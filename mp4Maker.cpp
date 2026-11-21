
#include <windows.h>
#include <iostream>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

bool createMp4FromImageAndAudio(
    const std::string& imagePath,
    const std::string& audioPath,
    const std::string& outputPath
) {
    // Validate input files
    if (!fs::exists(imagePath) || !fs::exists(audioPath)) {
        std::cerr << "Error: Input files do not exist.\n";
        return false;
    }

    // Construct FFmpeg command
    std::string cmd =
        "ffmpeg.exe -y -loop 1 -i \"" + imagePath + "\" "
        "-i \"" + audioPath + "\" "
        "-c:v libx264 -pix_fmt yuv420p -shortest \"" +
        outputPath + "\"";

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };

    std::string mutableCmd = cmd;

    // Launch FFmpeg
    if (!CreateProcessA(
        nullptr,
        mutableCmd.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    )) {
        std::cerr << "Failed to launch FFmpeg.\n";
        return false;
    }

    // Wait for encoding to finish
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return exitCode == 0;
}

int main() {
    std::string imagePath;
    std::string audioPath;
    std::string outputName;

    // Get user input
    std::cout << "Enter image path: ";
    std::getline(std::cin, imagePath);

    std::cout << "Enter audio path: ";
    std::getline(std::cin, audioPath);

    std::cout << "Enter output MP4 name: ";
    std::getline(std::cin, outputName);

    if (outputName.empty()) {
        std::cerr << "Output name cannot be empty.\n";
        return 1;
    }

    // Append .mp4 if not provided
    if (fs::path(outputName).extension() != ".mp4") {
        outputName += ".mp4";
    }

    std::cout << "\nCreating MP4...\n";

    if (createMp4FromImageAndAudio(
        imagePath,
        audioPath,
        outputName
    )) {
        std::cout << "Successfully created "
                  << outputName << "\n";
        return 0;
    }

    std::cerr << "Conversion failed.\n";
    return 1;
}
