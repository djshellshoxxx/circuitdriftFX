CircuitDrift for Windows (x64)

This archive contains two products: CircuitDrift and CircuitDrift LITE, each as VST3, CLAP and Standalone.

Status: the plugin shell, host parameters and state are in place; the audio engine is not wired in yet, so the
plugins pass audio through unchanged. Treat this build as a preview for host integration testing.

Install (copy, do not rename)
  VST3 : copy the two *.vst3 folders from VST3\ into  %COMMONPROGRAMFILES%\VST3
         (usually C:\Program Files\Common Files\VST3). Administrator rights are required.
  CLAP : copy the two *.clap files from CLAP\ into    %COMMONPROGRAMFILES%\CLAP
         (usually C:\Program Files\Common Files\CLAP). Administrator rights are required.
  Standalone : run the .exe files from Standalone\ directly; no installation is needed.

Per-user alternatives: %LOCALAPPDATA%\Programs\Common\VST3 and %LOCALAPPDATA%\Programs\Common\CLAP.
Rescan plugins in your host after copying. Remove the plugins by deleting the copied files.

Requirements: 64-bit Windows 10 or later and a 64-bit host. The binaries use the static C++ runtime; no
Visual C++ redistributable is needed. The binaries are not code signed, so Windows SmartScreen may warn.

Integrity: SHA256SUMS.txt lists the SHA-256 of every file in this archive. Verify with
  certutil -hashfile <file> SHA256
Licences: see LICENSE and THIRD-PARTY-NOTICES.md, and the licenses\ folder for the component licence texts.
