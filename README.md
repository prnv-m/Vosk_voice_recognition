# Audio Capture & Recognition Interface
This project is a real-time audio capture and recognition system integrating Vosk (for speech recognition) and PortAudio (for low-latency audio input/output). The goal is to build a complete audio processing pipeline, and current development efforts are focused on adding a WebRTC interface to enable real-time audio streaming over the web.
This will further be used in a project involving video interfaces.

## Features
✅ Live audio capture using PortAudio

✅ Offline speech recognition powered by Vosk

🔄 WebRTC integration (in progress) to enable remote access and real-time interaction

# Setup
Run make file to install all dependencies using:
```
make install-deps
make
./cppaudiocap
```

# Install a suitable vox model based on language and hardware constraints
Use this link: [Download]https://alphacephei.com/vosk/models


# Final file structure


├── lib/                     # External libraries (PortAudio, etc.)<br>
├── model/                 # Vosk speech recognition models<br>
├── cppaudiocap             # Compiled binary for local audio capture<br>
├── main.cpp          # Source code for audio processing<br>
├── makefile                # Build instructions<br>
└── README.md               # Project documentation<br>


