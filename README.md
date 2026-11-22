# mp4Maker

A small Windows app that turns a picture and an audio file into an MP4 video, for example to upload a song with its cover art. It is a simple window on top of [FFmpeg](https://ffmpeg.org/).

## Using it

1. Open `mp4Maker.exe`.
2. Choose a picture and an audio file, either with the **Browse** buttons or by dragging both files onto the window.
3. Check the **Save video as** name. It is filled in automatically next to the audio file, and you can change it.
4. Press **Create MP4**. The progress bar shows how far along it is, and **Cancel** stops it.
5. When it finishes, **Show in folder** opens the folder with the new video selected.

The video lasts as long as the audio and shows the picture the whole time.

Supported pictures: PNG, JPG, BMP, WebP, TIFF.
Supported audio: MP3, WAV, M4A, AAC, FLAC, OGG, Opus, WMA.
Other types FFmpeg understands can be picked through **Browse** by switching the filter to "All files".

## Requirements

- Windows 10 or later
- FFmpeg, either on your `PATH` or as `ffmpeg.exe` in the same folder as `mp4Maker.exe`

To install FFmpeg, run this in a Command Prompt and then reopen mp4Maker:

```
winget install Gyan.FFmpeg
```

## Building

You need [LLVM](https://llvm.org/) (`clang++` and `llvm-rc`) and the Visual Studio C++ build tools with the Windows SDK. Then run:

```
build.bat
```

This produces `mp4Maker.exe` in the project folder.

## Files

| File | Purpose |
| --- | --- |
| `mp4Maker.cpp` | The whole program: the window and the FFmpeg call |
| `mp4Maker.manifest` | Turns on modern control styling and high-DPI support |
| `mp4Maker.rc` | Embeds the manifest into the exe |
| `build.bat` | Build script |
