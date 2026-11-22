// mp4Maker - turns a picture and an audio file into an MP4 video using FFmpeg.
//
// Build: run build.bat (needs clang++ and llvm-rc; ffmpeg.exe must be on PATH
// or next to mp4Maker.exe when the program runs).

#define _CRT_SECURE_NO_WARNINGS
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define NOMINMAX

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <atomic>
#include <cstdio>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

enum ControlId : int {
    IDC_IMAGE_EDIT = 101,
    IDC_IMAGE_BROWSE,
    IDC_AUDIO_EDIT,
    IDC_AUDIO_BROWSE,
    IDC_OUTPUT_EDIT,
    IDC_OUTPUT_BROWSE,
    IDC_SHOW_IN_FOLDER,
};

// Posted by the encoding thread to the main window.
const UINT WM_APP_PROGRESS = WM_APP + 1;  // wParam: progress, 0..1000
const UINT WM_APP_DONE     = WM_APP + 2;  // wParam: EncodeResult, lParam: std::wstring* (error text)

enum class EncodeResult { Success, Failed, Cancelled };

struct EncodeJob {
    std::wstring ffmpeg;
    std::wstring image;
    std::wstring audio;
    std::wstring output;
};

struct FileRow {
    HWND label  = nullptr;
    HWND edit   = nullptr;
    HWND browse = nullptr;
};

struct App {
    HWND hwnd = nullptr;
    FileRow image, audio, output;
    HWND hint      = nullptr;
    HWND progress  = nullptr;
    HWND status    = nullptr;
    HWND showBtn   = nullptr;
    HWND createBtn = nullptr;
    HWND cancelBtn = nullptr;
    HWND lastFocus = nullptr;

    HFONT font   = nullptr;
    UINT  dpi    = 96;
    int   fontPx = 12;  // UI font height in pixels; the layout scales with it

    // FFmpeg runs inside this job so it can be stopped, and so it never
    // outlives this program.
    HANDLE job = nullptr;
    std::thread worker;
    std::atomic<bool> cancel{false};
    bool busy          = false;
    bool marquee       = false;
    bool closeWhenDone = false;

    std::wstring currentOutput;    // file being created right now
    std::wstring finishedOutput;   // last file created successfully
    std::wstring suggestedOutput;  // output name we filled in automatically
    std::wstring overwriteOkFor;   // path the Save dialog already confirmed replacing
};

static App g;

static const wchar_t* const kAppName = L"mp4Maker";

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static std::wstring quote(const std::wstring& s) { return L"\"" + s + L"\""; }

static std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(), n);
    return out;
}

static std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

// Trims whitespace and the quotes Explorer's "Copy as path" puts around a path.
static std::wstring cleanPath(std::wstring s) {
    auto trim = [](std::wstring& t) {
        size_t a = t.find_first_not_of(L" \t\r\n");
        size_t b = t.find_last_not_of(L" \t\r\n");
        t = (a == std::wstring::npos) ? L"" : t.substr(a, b - a + 1);
    };
    trim(s);
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') {
        s = s.substr(1, s.size() - 2);
        trim(s);
    }
    return s;
}

static bool isFile(const std::wstring& path) {
    std::error_code ec;
    return !path.empty() && fs::is_regular_file(fs::path(path), ec);
}

static std::wstring absolutePath(const std::wstring& path) {
    std::error_code ec;
    fs::path abs = fs::absolute(fs::path(path), ec);
    return ec ? path : abs.lexically_normal().wstring();
}

static std::wstring getText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, L'\0');
    if (n > 0) GetWindowTextW(h, s.data(), n + 1);
    return s;
}

// Sets a path into an edit box, scrolled so the file name at the end is visible.
static void setPathText(HWND edit, const std::wstring& path) {
    SetWindowTextW(edit, path.c_str());
    SendMessageW(edit, EM_SETSEL, path.size(), path.size());
}

static void setStatus(const std::wstring& text) { SetWindowTextW(g.status, text.c_str()); }

static void showError(const std::wstring& text) {
    MessageBoxW(g.hwnd, text.c_str(), kAppName, MB_OK | MB_ICONWARNING);
}

// ---------------------------------------------------------------------------
// Encoding (runs on a worker thread)
// ---------------------------------------------------------------------------

static std::wstring buildCommandLine(const EncodeJob& job, const std::wstring& tempOutput) {
    // FFmpeg reads a '%' in the picture's path as an image-sequence pattern
    // (like frame%d.png). Only its "image2" reader can switch that off, so use
    // it for those paths. Every other picture is left to FFmpeg's normal
    // detection, whose readers reject the pattern_type option but recognise a
    // picture by its contents, even with the wrong file extension.
    const bool hasPattern = job.image.find(L'%') != std::wstring::npos;

    return quote(job.ffmpeg) +
        // "repeat" stops FFmpeg collapsing repeated log lines, which the
        // unreadable-picture check in runEncode relies on.
        L" -hide_banner -nostdin -y -loglevel repeat+info" +
        (hasPattern ? L" -f image2 -pattern_type none" : L"") +
        L" -loop 1 -i " + quote(job.image) +
        L" -i " + quote(job.audio) +
        // Picture from the image, sound from the audio file (which may carry
        // its own embedded cover art that we don't want).
        L" -map 0:v:0 -map 1:a:0"
        L" -c:v libx264 -tune stillimage -pix_fmt yuv420p"
        // yuv420p needs even dimensions; round odd-sized pictures down by a pixel.
        L" -vf \"scale=trunc(iw/2)*2:trunc(ih/2)*2\""
        L" -shortest -f mp4 " + quote(tempOutput);
}

// Parses "HH:MM:SS.ff"; returns seconds, or -1 if the text isn't a time.
static double parseClock(const char* s) {
    int h = 0, m = 0;
    double sec = 0;
    if (std::sscanf(s, "%d:%d:%lf", &h, &m, &sec) != 3 || h < 0 || m < 0 || sec < 0) return -1;
    return h * 3600.0 + m * 60.0 + sec;
}

static void runEncode(EncodeJob job) {
    // Encode to a temporary file so an existing video is only replaced once
    // the new one is complete.
    const std::wstring temp = job.output + L".mp4maker-tmp";

    auto finish = [&](EncodeResult result, const std::wstring& errorText) {
        if (result != EncodeResult::Success) DeleteFileW(temp.c_str());
        PostMessageW(g.hwnd, WM_APP_DONE, (WPARAM)result, (LPARAM) new std::wstring(errorText));
    };

    // FFmpeg reports everything on stderr; capture it through a pipe.
    SECURITY_ATTRIBUTES inherit = { sizeof(inherit), nullptr, TRUE };
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &inherit, 0)) {
        finish(EncodeResult::Failed, L"Couldn't set up communication with FFmpeg.");
        return;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                             OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = nul;
    si.hStdOutput = nul;
    si.hStdError  = writeEnd;
    PROCESS_INFORMATION pi = {};

    std::wstring cmd = buildCommandLine(job, temp);
    BOOL started = CreateProcessW(job.ffmpeg.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
    DWORD startError = GetLastError();
    CloseHandle(writeEnd);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);

    if (!started) {
        CloseHandle(readEnd);
        finish(EncodeResult::Failed,
               L"FFmpeg couldn't be started (error " + std::to_wstring(startError) + L").");
        return;
    }

    // Cancelling works by terminating the job, so the process must be in it
    // before it is allowed to run.
    if (!AssignProcessToJobObject(g.job, pi.hProcess) || g.cancel) {
        bool cancelled = g.cancel;
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(readEnd);
        finish(cancelled ? EncodeResult::Cancelled : EncodeResult::Failed,
               L"FFmpeg couldn't be started.");
        return;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    double duration = 0;           // length of the audio, once FFmpeg reports it
    std::deque<std::string> tail;  // last few log lines, shown if encoding fails
    int pictureErrors = 0;         // failed attempts to decode the picture
    bool pictureUnreadable = false;

    auto handleLine = [&](const std::string& line) {
        if (line.rfind("frame=", 0) == 0 || line.rfind("size=", 0) == 0) {
            // Periodic status line: "frame=  123 fps=... time=00:00:04.92 ..."
            size_t p = line.find("time=");
            if (p != std::string::npos && duration > 0) {
                double t = parseClock(line.c_str() + p + 5);
                if (t >= 0) {
                    double fraction = t / duration;
                    if (fraction > 1) fraction = 1;
                    PostMessageW(g.hwnd, WM_APP_PROGRESS, (WPARAM)(fraction * 1000), 0);
                }
            }
            return;
        }
        // With -loop, FFmpeg retries a picture it can't decode forever instead
        // of giving up, so stop it once that is clearly what is happening.
        // (Input 0, stream 0 is the picture; the wording differs by version.)
        if (line.find("#0:0") != std::string::npos &&
            (line.find("Decoding error") != std::string::npos ||
             line.find("Error while decoding") != std::string::npos)) {
            if (++pictureErrors == 25) {
                pictureUnreadable = true;
                TerminateProcess(pi.hProcess, 1);
            }
            return;
        }
        // The picture reports a duration of one frame; the audio is the long one.
        size_t p = line.find("Duration: ");
        if (p != std::string::npos) {
            double d = parseClock(line.c_str() + p + 10);
            if (d > duration) duration = d;
        }
        tail.push_back(line.substr(0, 300));
        if (tail.size() > 8) tail.pop_front();
    };

    std::string pending;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) && got > 0) {
        for (DWORD i = 0; i < got; ++i) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (!pending.empty()) handleLine(pending);
                pending.clear();
            } else if (pending.size() < 8192) {
                pending.push_back(c);
            }
        }
    }
    if (!pending.empty()) handleLine(pending);
    CloseHandle(readEnd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);

    if (g.cancel) {
        finish(EncodeResult::Cancelled, L"");
    } else if (pictureUnreadable) {
        finish(EncodeResult::Failed,
               L"The picture couldn't be read. It may be damaged, or not really a picture "
               L"file:\n\n" + job.image);
    } else if (exitCode != 0) {
        std::string log;
        for (const std::string& line : tail) log += line + "\n";
        finish(EncodeResult::Failed,
               L"The video couldn't be made. Check that the picture and audio files open "
               L"normally in other programs.\n\nDetails from FFmpeg:\n\n" + widen(log));
    } else if (!MoveFileExW(temp.c_str(), job.output.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        finish(EncodeResult::Failed,
               L"The video was made, but it couldn't be saved as:\n" + job.output +
               L"\n\nIf that file is open in another program, close it and try again.");
    } else {
        finish(EncodeResult::Success, L"");
    }
}

// ---------------------------------------------------------------------------
// Window layout
// ---------------------------------------------------------------------------

// Scales a measurement designed for the standard 9pt UI font to the real font,
// so the window follows both the monitor's DPI and the user's text size.
static int S(int v) { return MulDiv(v, g.fontPx, 12); }

static HFONT createUiFont() {
    LOGFONTW lf = {};
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g.dpi)) {
        lf = ncm.lfMessageFont;
    } else {
        lf.lfHeight = -MulDiv(9, g.dpi, 72);
        wcscpy(lf.lfFaceName, L"Segoe UI");
    }
    g.fontPx = lf.lfHeight < 0 ? -lf.lfHeight : lf.lfHeight;
    if (g.fontPx == 0) g.fontPx = MulDiv(12, g.dpi, 96);
    return CreateFontIndirectW(&lf);
}

// Positions every control and returns the client area size they need.
static SIZE layoutControls() {
    const int W = S(580), M = S(16), gap = S(8);
    const int inner = W - 2 * M;
    const int btnW = S(96), btnH = S(28), editH = S(24), labelH = S(18);

    int y = S(14);
    for (FileRow* row : { &g.image, &g.audio, &g.output }) {
        MoveWindow(row->label, M, y, inner, labelH, TRUE);
        y += labelH + S(2);
        MoveWindow(row->edit, M, y + (btnH - editH) / 2, inner - btnW - gap, editH, TRUE);
        MoveWindow(row->browse, W - M - btnW, y, btnW, btnH, TRUE);
        y += btnH + S(10);
    }

    MoveWindow(g.hint, M, y, inner, labelH, TRUE);
    y += labelH + S(16);

    MoveWindow(g.progress, M, y, inner, S(16), TRUE);
    y += S(16) + S(8);
    MoveWindow(g.status, M, y, inner, labelH * 2, TRUE);
    y += labelH * 2 + S(12);

    const int createW = S(120);
    MoveWindow(g.showBtn, M, y, S(140), btnH, TRUE);
    MoveWindow(g.createBtn, W - M - btnW - gap - createW, y, createW, btnH, TRUE);
    MoveWindow(g.cancelBtn, W - M - btnW, y, btnW, btnH, TRUE);
    y += btnH + S(16);

    return { W, y };
}

// (Re)creates the font and lays the window out for the current DPI. With no
// suggested position the window is centred on its monitor.
static void applyDpi(const RECT* suggested) {
    HFONT old = g.font;
    g.font = createUiFont();
    EnumChildWindows(g.hwnd, [](HWND child, LPARAM font) -> BOOL {
        SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
        return TRUE;
    }, (LPARAM)g.font);
    if (old) DeleteObject(old);

    SIZE client = layoutControls();
    RECT r = { 0, 0, client.cx, client.cy };
    AdjustWindowRectExForDpi(&r, (DWORD)GetWindowLongPtrW(g.hwnd, GWL_STYLE), FALSE,
                             (DWORD)GetWindowLongPtrW(g.hwnd, GWL_EXSTYLE), g.dpi);
    int w = r.right - r.left, h = r.bottom - r.top;

    int x, y;
    if (suggested) {
        x = suggested->left;
        y = suggested->top;
    } else {
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
        y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
    }
    SetWindowPos(g.hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

static HWND makeControl(const wchar_t* cls, const wchar_t* text, DWORD style, int id,
                        DWORD exStyle = 0) {
    return CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                           g.hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
}

// A label, a path box and a Browse button. The label comes first so its
// Alt+letter shortcut moves the keyboard focus into the path box.
static FileRow makeFileRow(const wchar_t* label, const wchar_t* placeholder, int editId,
                           int browseId) {
    FileRow row;
    row.label  = makeControl(L"STATIC", label, SS_LEFT, -1);
    row.edit   = makeControl(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, editId, WS_EX_CLIENTEDGE);
    row.browse = makeControl(L"BUTTON", L"Browse\u2026", WS_TABSTOP | BS_PUSHBUTTON, browseId);
    SendMessageW(row.edit, EM_SETCUEBANNER, FALSE, (LPARAM)placeholder);
    SendMessageW(row.edit, EM_SETLIMITTEXT, 32767, 0);
    return row;
}

static void createControls() {
    g.image = makeFileRow(L"&Picture:", L"Choose a picture (PNG, JPG, BMP\u2026)",
                          IDC_IMAGE_EDIT, IDC_IMAGE_BROWSE);
    g.audio = makeFileRow(L"&Audio file:", L"Choose a sound file (MP3, WAV, M4A\u2026)",
                          IDC_AUDIO_EDIT, IDC_AUDIO_BROWSE);
    g.output = makeFileRow(L"&Save video as:", L"Where to save the finished MP4",
                           IDC_OUTPUT_EDIT, IDC_OUTPUT_BROWSE);

    g.hint = makeControl(L"STATIC",
                         L"Tip: you can also drag a picture and an audio file onto this window.",
                         SS_LEFT | SS_NOPREFIX, -1);
    g.progress = makeControl(PROGRESS_CLASSW, L"", 0, -1);
    SendMessageW(g.progress, PBM_SETRANGE32, 0, 1000);
    g.status = makeControl(L"STATIC",
                           L"Choose a picture and an audio file, then press Create MP4.",
                           SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL, -1);

    g.createBtn = makeControl(L"BUTTON", L"&Create MP4", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
    g.cancelBtn = makeControl(L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED,
                              IDCANCEL);
    g.showBtn = makeControl(L"BUTTON", L"Show in &folder", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED,
                            IDC_SHOW_IN_FOLDER);
}

// ---------------------------------------------------------------------------
// Choosing files
// ---------------------------------------------------------------------------

static const wchar_t kImageFilter[] =
    L"Pictures (*.png;*.jpg;*.jpeg;*.bmp;*.webp;*.tif;*.tiff)\0"
    L"*.png;*.jpg;*.jpeg;*.bmp;*.webp;*.tif;*.tiff\0"
    L"All files (*.*)\0*.*\0";
static const wchar_t kAudioFilter[] =
    L"Audio (*.mp3;*.wav;*.m4a;*.aac;*.flac;*.ogg;*.opus;*.wma)\0"
    L"*.mp3;*.wav;*.m4a;*.aac;*.flac;*.ogg;*.opus;*.wma\0"
    L"All files (*.*)\0*.*\0";
static const wchar_t kVideoFilter[] = L"MP4 video (*.mp4)\0*.mp4\0";

enum class FileKind { Image, Audio, Unknown };

static FileKind classify(const std::wstring& path) {
    static const wchar_t* const images[] = { L".png", L".jpg", L".jpeg", L".bmp", L".webp",
                                             L".tif", L".tiff" };
    static const wchar_t* const audio[] = { L".mp3", L".wav", L".m4a", L".aac", L".flac",
                                            L".ogg", L".opus", L".wma" };
    std::wstring ext = lower(fs::path(path).extension().wstring());
    for (const wchar_t* e : images) if (ext == e) return FileKind::Image;
    for (const wchar_t* e : audio)  if (ext == e) return FileKind::Audio;
    return FileKind::Unknown;
}

// Shows an Open or Save dialog starting from the folder of `current`.
static bool pickFile(bool save, const wchar_t* title, const wchar_t* filter,
                     const std::wstring& current, std::wstring& result) {
    std::vector<wchar_t> file(32768, L'\0');
    std::wstring initialDir;
    if (!current.empty()) {
        std::error_code ec;
        fs::path cur(current);
        if (fs::is_directory(cur.parent_path(), ec)) initialDir = cur.parent_path().wstring();
        if (save) wcsncpy(file.data(), cur.filename().c_str(), file.size() - 1);
    }

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrTitle = title;
    ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
    ofn.Flags = OFN_NOCHANGEDIR | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST |
                (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (save) ofn.lpstrDefExt = L"mp4";

    auto show = [&] { return save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn); };
    BOOL ok = show();
    if (!ok && CommDlgExtendedError() == FNERR_INVALIDFILENAME) {
        // The name carried over from the text box wasn't usable; start blank.
        file[0] = L'\0';
        ok = show();
    }
    if (!ok) return false;
    result = file.data();
    return true;
}

// Picks "<audio name>.mp4" beside the audio file, avoiding existing files.
static std::wstring suggestOutputFor(const std::wstring& audio) {
    fs::path a(audio);
    fs::path dir = a.parent_path();
    std::wstring stem = a.stem().wstring();
    for (int n = 1; n < 1000; ++n) {
        std::wstring name = (n == 1) ? stem + L".mp4"
                                     : stem + L" (" + std::to_wstring(n) + L").mp4";
        std::error_code ec;
        if (!fs::exists(dir / name, ec)) return (dir / name).wstring();
    }
    return (dir / (stem + L".mp4")).wstring();
}

// Fills in the output box from the audio file, unless the user chose a name.
static void maybeSuggestOutput() {
    std::wstring current = cleanPath(getText(g.output.edit));
    if (!current.empty() && current != g.suggestedOutput) return;
    std::wstring audio = cleanPath(getText(g.audio.edit));
    if (!isFile(audio)) return;
    g.suggestedOutput = suggestOutputFor(absolutePath(audio));
    setPathText(g.output.edit, g.suggestedOutput);
}

static void onBrowse(int id) {
    std::wstring path;
    if (id == IDC_IMAGE_BROWSE) {
        if (!pickFile(false, L"Choose a picture", kImageFilter,
                      cleanPath(getText(g.image.edit)), path)) return;
        setPathText(g.image.edit, path);
        SetFocus(g.image.edit);
    } else if (id == IDC_AUDIO_BROWSE) {
        if (!pickFile(false, L"Choose an audio file", kAudioFilter,
                      cleanPath(getText(g.audio.edit)), path)) return;
        setPathText(g.audio.edit, path);
        SetFocus(g.audio.edit);
        maybeSuggestOutput();
    } else {
        if (!pickFile(true, L"Save video as", kVideoFilter,
                      cleanPath(getText(g.output.edit)), path)) return;
        setPathText(g.output.edit, path);
        SetFocus(g.output.edit);
        g.overwriteOkFor = path;  // the dialog already asked about replacing it
    }
}

static void onDropFiles(HDROP drop) {
    UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    bool unknown = false;
    for (UINT i = 0; i < count; ++i) {
        UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring path(len, L'\0');
        DragQueryFileW(drop, i, path.data(), len + 1);
        switch (classify(path)) {
            case FileKind::Image: setPathText(g.image.edit, path); break;
            case FileKind::Audio: setPathText(g.audio.edit, path); break;
            default:              unknown = true; break;
        }
    }
    DragFinish(drop);
    maybeSuggestOutput();
    if (unknown) {
        setStatus(L"That file type wasn't recognised as a picture or audio. "
                  L"You can still pick it with a Browse button.");
    }
    SetForegroundWindow(g.hwnd);
}

// ---------------------------------------------------------------------------
// Starting, stopping and finishing an encode
// ---------------------------------------------------------------------------

static void setMarquee(bool on) {
    if (g.marquee == on) return;
    g.marquee = on;
    LONG_PTR style = GetWindowLongPtrW(g.progress, GWL_STYLE);
    if (on) {
        SetWindowLongPtrW(g.progress, GWL_STYLE, style | PBS_MARQUEE);
        SendMessageW(g.progress, PBM_SETMARQUEE, TRUE, 30);
    } else {
        SendMessageW(g.progress, PBM_SETMARQUEE, FALSE, 0);
        SetWindowLongPtrW(g.progress, GWL_STYLE, style & ~PBS_MARQUEE);
    }
}

static void setBusy(bool busy) {
    g.busy = busy;
    for (FileRow* row : { &g.image, &g.audio, &g.output }) {
        EnableWindow(row->edit, !busy);
        EnableWindow(row->browse, !busy);
    }
    EnableWindow(g.createBtn, !busy);
    EnableWindow(g.cancelBtn, busy);
    DragAcceptFiles(g.hwnd, !busy);
    if (!busy) SetWindowTextW(g.hwnd, kAppName);
}

// Shows a message about one of the boxes and puts the cursor in it.
static void complain(HWND edit, const std::wstring& message) {
    showError(message);
    SetFocus(edit);
    SendMessageW(edit, EM_SETSEL, 0, -1);
}

static void startEncode() {
    if (g.busy) return;

    std::wstring image = cleanPath(getText(g.image.edit));
    if (image.empty()) return complain(g.image.edit, L"Choose a picture first.");
    if (!isFile(image)) return complain(g.image.edit, L"This picture can't be found:\n\n" + image);

    std::wstring audio = cleanPath(getText(g.audio.edit));
    if (audio.empty()) return complain(g.audio.edit, L"Choose an audio file first.");
    if (!isFile(audio)) return complain(g.audio.edit, L"This audio file can't be found:\n\n" + audio);

    image = absolutePath(image);
    audio = absolutePath(audio);

    maybeSuggestOutput();
    std::wstring typed = cleanPath(getText(g.output.edit));
    if (typed.empty()) return complain(g.output.edit, L"Choose where to save the video.");

    std::error_code ec;
    fs::path out(typed);
    if (!out.has_filename() || fs::is_directory(out, ec)) {
        return complain(g.output.edit,
                        L"That is a folder. Add a name for the video, for example:\n\n" +
                        (out / L"my video.mp4").wstring());
    }
    if (lower(out.extension().wstring()) != L".mp4") out += L".mp4";
    // A bare name like "holiday" is saved next to the audio file.
    if (!out.has_root_path() && !out.has_parent_path()) out = fs::path(audio).parent_path() / out;
    std::wstring output = absolutePath(out.wstring());
    out = fs::path(output);

    if (!fs::is_directory(out.parent_path(), ec)) {
        return complain(g.output.edit,
                        L"This folder doesn't exist:\n\n" + out.parent_path().wstring());
    }
    if (fs::equivalent(out, fs::path(image), ec) || fs::equivalent(out, fs::path(audio), ec)) {
        return complain(g.output.edit,
                        L"The video can't be saved over the picture or audio file it is made "
                        L"from. Choose a different name.");
    }
    if (fs::exists(out, ec) && output != g.overwriteOkFor) {
        std::wstring question = output + L"\n\nThis file already exists. Replace it?";
        if (MessageBoxW(g.hwnd, question.c_str(), kAppName,
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
            SetFocus(g.output.edit);
            return;
        }
    }
    g.overwriteOkFor.clear();

    std::vector<wchar_t> found(32768, L'\0');
    DWORD n = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, (DWORD)found.size(), found.data(), nullptr);
    if (n == 0 || n >= found.size()) {
        showError(L"FFmpeg wasn't found.\n\n"
                  L"mp4Maker uses the free FFmpeg program to make videos. To install it, open "
                  L"a Command Prompt and run:\n\n"
                  L"    winget install Gyan.FFmpeg\n\n"
                  L"Then close and reopen mp4Maker. (Alternatively, put ffmpeg.exe in the same "
                  L"folder as mp4Maker.exe.)");
        return;
    }

    // Show the tidied-up paths that will actually be used.
    if (getText(g.image.edit) != image) setPathText(g.image.edit, image);
    if (getText(g.audio.edit) != audio) setPathText(g.audio.edit, audio);
    if (typed != output) {
        if (typed == g.suggestedOutput) g.suggestedOutput = output;
        setPathText(g.output.edit, output);
    }

    g.cancel = false;
    g.currentOutput = output;
    SendMessageW(g.progress, PBM_SETPOS, 0, 0);
    setMarquee(true);
    setStatus(L"Getting started\u2026");
    setBusy(true);
    EnableWindow(g.showBtn, FALSE);
    SetFocus(g.cancelBtn);

    if (g.worker.joinable()) g.worker.join();
    g.worker = std::thread(runEncode, EncodeJob{ found.data(), image, audio, output });
}

static void requestCancel() {
    if (!g.busy || g.cancel) return;
    g.cancel = true;
    TerminateJobObject(g.job, 1);
    setStatus(L"Stopping\u2026");
}

static void onProgress(int permille) {
    if (!g.busy || g.cancel) return;
    setMarquee(false);
    SendMessageW(g.progress, PBM_SETPOS, permille, 0);
    std::wstring percent = std::to_wstring(permille / 10) + L"%";
    setStatus(L"Creating video\u2026 " + percent);
    SetWindowTextW(g.hwnd, (percent + L" \u2013 " + kAppName).c_str());
}

static void onDone(EncodeResult result, const std::wstring& errorText) {
    if (g.worker.joinable()) g.worker.join();
    setMarquee(false);
    setBusy(false);

    if (g.closeWhenDone) {
        DestroyWindow(g.hwnd);
        return;
    }

    bool ok = (result == EncodeResult::Success);
    SendMessageW(g.progress, PBM_SETPOS, ok ? 1000 : 0, 0);
    EnableWindow(g.showBtn, ok);

    switch (result) {
        case EncodeResult::Success:
            g.finishedOutput = g.currentOutput;
            setStatus(L"Done! Your video was saved as:\n" + g.finishedOutput);
            SetFocus(g.showBtn);
            MessageBeep(MB_ICONINFORMATION);
            if (GetForegroundWindow() != g.hwnd) FlashWindow(g.hwnd, TRUE);
            break;
        case EncodeResult::Cancelled:
            setStatus(L"Cancelled. No video was made.");
            SetFocus(g.createBtn);
            break;
        case EncodeResult::Failed:
            setStatus(L"The video couldn't be made.");
            SetFocus(g.createBtn);
            showError(errorText);
            break;
    }
}

static void showInFolder() {
    if (g.finishedOutput.empty()) return;
    std::wstring args = L"/select," + quote(g.finishedOutput);
    ShellExecuteW(g.hwnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            g.hwnd = hwnd;
            g.dpi = GetDpiForWindow(hwnd);
            createControls();
            applyDpi(nullptr);
            DragAcceptFiles(hwnd, TRUE);
            return 0;

        case WM_DPICHANGED:
            g.dpi = HIWORD(wParam);
            applyDpi((const RECT*)lParam);
            return 0;

        case WM_COMMAND: {
            int id = LOWORD(wParam), code = HIWORD(wParam);
            if (id == IDC_AUDIO_EDIT && code == EN_KILLFOCUS) {
                maybeSuggestOutput();
            } else if (code == BN_CLICKED) {
                switch (id) {
                    case IDC_IMAGE_BROWSE:
                    case IDC_AUDIO_BROWSE:
                    case IDC_OUTPUT_BROWSE:   onBrowse(id); break;
                    case IDOK:                startEncode(); break;   // also Enter
                    case IDCANCEL:            requestCancel(); break; // also Esc
                    case IDC_SHOW_IN_FOLDER:  showInFolder(); break;
                }
            }
            return 0;
        }

        case WM_DROPFILES:
            if (g.busy) DragFinish((HDROP)wParam);
            else onDropFiles((HDROP)wParam);
            return 0;

        case WM_APP_PROGRESS:
            onProgress((int)wParam);
            return 0;

        case WM_APP_DONE: {
            std::unique_ptr<std::wstring> errorText((std::wstring*)lParam);
            onDone((EncodeResult)wParam, *errorText);
            return 0;
        }

        // Remember which control had the keyboard focus so it comes back when
        // the user switches away and returns.
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) {
                HWND focus = GetFocus();
                if (focus && IsChild(hwnd, focus)) g.lastFocus = focus;
            }
            return 0;

        case WM_SETFOCUS:
            if (g.lastFocus && IsWindowEnabled(g.lastFocus)) SetFocus(g.lastFocus);
            else SetFocus(g.busy ? g.cancelBtn : g.image.edit);
            return 0;

        case WM_CLOSE:
            if (g.busy) {
                int answer = MessageBoxW(hwnd,
                    L"A video is still being made.\n\nStop it and close mp4Maker?", kAppName,
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
                if (answer != IDYES) return 0;
                if (g.busy) {  // may have finished while the question was showing
                    g.closeWhenDone = true;
                    requestCancel();
                    return 0;
                }
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (g.worker.joinable()) {
                g.cancel = true;
                TerminateJobObject(g.job, 1);
                g.worker.join();
            }
            if (g.font) DeleteObject(g.font);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCmd) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    g.job = CreateJobObjectW(nullptr, nullptr);
    if (g.job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g.job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"mp4MakerWindow";
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, kAppName,
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 600, 400,
                                nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, showCmd);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // IsDialogMessage provides Tab, Enter, Esc and Alt+letter navigation.
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (g.job) CloseHandle(g.job);
    return (int)msg.wParam;
}
