#include <iostream>
#include <cstring>
#include <vector>
#include <atomic> // For std::atomic_bool
#include <thread>   // For input thread
#include <chrono>   // For std::this_thread::sleep_for

// Vosk API (expected in D:\vsk\)
#include "vosk_api.h"

// PortAudio API (expected in D:\vsk\)
#include <portaudio.h>

// --- Configuration ---
const char *MODEL_PATH = "/mnt/d/vsk/model"; // Absolute path to your model

                                    // Or use "D:\\model" with escaped backslashes

#define SAMPLE_RATE         (16000)   // Standard for most Vosk models
#define FRAMES_PER_BUFFER   (1024)    // Number of audio frames per buffer, affects latency
#define NUM_CHANNELS        (1)       // Mono
#define PA_SAMPLE_TYPE      (paInt16) // Vosk expects 16-bit PCM
// --- End Configuration ---

// Global flag to signal threads to stop
std::atomic<bool> g_request_stop(false);
std::string g_last_partial_result_json; // To avoid printing duplicate partials

// PortAudio callback function: Called by PortAudio thread to process audio
static int paCallback(const void *inputBuffer, void *outputBuffer,
                      unsigned long framesPerBuffer,
                      const PaStreamCallbackTimeInfo* timeInfo,
                      PaStreamCallbackFlags statusFlags,
                      void *userData) // User data is the VoskRecognizer
{
    VoskRecognizer *recognizer = (VoskRecognizer*)userData;
    const short *audio_data = (const short*)inputBuffer;

    if (g_request_stop) {
        return paComplete; // Tell PortAudio to stop calling this callback
    }

    if (inputBuffer == NULL) {
        return paContinue; // No input, just continue
    }

    // Feed audio data to Vosk
    // vosk_recognizer_accept_waveform_s returns:
    //   1 if a silence is detected and a final result is available
    //   0 if more data is needed or an intermediate (partial) result is available
    //  -1 on error
    int vosk_status = vosk_recognizer_accept_waveform_s(recognizer, audio_data, framesPerBuffer);

    if (vosk_status == 0) { // Partial result
        const char* partial_json_cstr = vosk_recognizer_partial_result(recognizer);
        if (partial_json_cstr && strlen(partial_json_cstr) > 0) {
            std::string current_partial_json(partial_json_cstr);
            // Avoid printing empty partials like {"partial" : ""} or identical subsequent partials
            if (current_partial_json.find("\"partial\" : \"\"") == std::string::npos &&
                current_partial_json != g_last_partial_result_json) {
                std::cout << "Partial: " << current_partial_json << std::endl;
                g_last_partial_result_json = current_partial_json;
            }
        }
    } else if (vosk_status > 0) { // Final result (vosk_status == 1)
        const char* final_result_json_cstr = vosk_recognizer_result(recognizer);
        if (final_result_json_cstr && strlen(final_result_json_cstr) > 0) {
            std::cout << "Final:   " << final_result_json_cstr << std::endl;
        }
        g_last_partial_result_json.clear(); // Reset for next utterance
    }
    // Negative vosk_status indicates an error, not explicitly handled here for brevity

    return paContinue; // Tell PortAudio to keep calling this callback
}

// Thread function to listen for 'q' key press to quit
void checkForQuitCommand() {
    std::cout << "\nMic is active. Live input will be shown below." << std::endl;
    std::cout << ">>> Type 'q' and press Enter to stop recording. <<<\n" << std::endl;
    char c;
    while (std::cin.get(c)) { // Reads one character at a time
        if (c == 'q' || c == 'Q') {
            g_request_stop = true; // Signal other threads to stop
            break;
        }
        // If 'q' was entered, and then Enter, the '\n' will be consumed next.
        // If g_request_stop is true, this ensures the loop exits after Enter.
        if (c == '\n' && g_request_stop) {
            break;
        }
    }
}

int main() {
    // 1. Initialize Vosk Model
    VoskModel *model = vosk_model_new(MODEL_PATH);
    if (!model) {
        std::cerr << "ERROR: Failed to load Vosk model from \"" << MODEL_PATH << "\"" << std::endl;
        std::cerr << "Please ensure the path is correct and model files are present." << std::endl;
        return 1;
    }
    std::cout << "Vosk model loaded successfully." << std::endl;

    // 2. Create Vosk Recognizer
    VoskRecognizer *recognizer = vosk_recognizer_new(model, (float)SAMPLE_RATE);
    if (!recognizer) {
        std::cerr << "ERROR: Failed to create Vosk recognizer." << std::endl;
        vosk_model_free(model);
        return 1;
    }
    // Optional: For word-level timestamps in results (adds detail to JSON)
    // vosk_recognizer_set_words(recognizer, 1);

    // 3. Initialize PortAudio
    PaError pa_err = Pa_Initialize();
    if (pa_err != paNoError) {
        std::cerr << "PortAudio ERROR: Pa_Initialize returned: " << Pa_GetErrorText(pa_err) << std::endl;
        vosk_recognizer_free(recognizer);
        vosk_model_free(model);
        return 1;
    }

    // 4. Set up PortAudio Stream Parameters
    PaStreamParameters inputParameters;
    inputParameters.device = Pa_GetDefaultInputDevice(); // Use default microphone
    if (inputParameters.device == paNoDevice) {
        std::cerr << "PortAudio ERROR: No default input device found." << std::endl;
        Pa_Terminate();
        vosk_recognizer_free(recognizer);
        vosk_model_free(model);
        return 1;
    }
    inputParameters.channelCount = NUM_CHANNELS;
    inputParameters.sampleFormat = PA_SAMPLE_TYPE;
    inputParameters.suggestedLatency = Pa_GetDeviceInfo(inputParameters.device)->defaultLowInputLatency;
    inputParameters.hostApiSpecificStreamInfo = NULL;

    // 5. Open PortAudio Stream
    PaStream *pa_stream;
    pa_err = Pa_OpenStream(
                 &pa_stream,
                 &inputParameters,
                 NULL, // No output stream parameters
                 SAMPLE_RATE,
                 FRAMES_PER_BUFFER,
                 paClipOff, // No audio clipping
                 paCallback,  // Your callback function
                 recognizer); // Pass recognizer to the callback

    if (pa_err != paNoError) {
        std::cerr << "PortAudio ERROR: Pa_OpenStream returned: " << Pa_GetErrorText(pa_err) << std::endl;
        Pa_Terminate();
        vosk_recognizer_free(recognizer);
        vosk_model_free(model);
        return 1;
    }

    // 6. Start PortAudio Stream (begins calling paCallback)
    pa_err = Pa_StartStream(pa_stream);
    if (pa_err != paNoError) {
        std::cerr << "PortAudio ERROR: Pa_StartStream returned: " << Pa_GetErrorText(pa_err) << std::endl;
        Pa_CloseStream(pa_stream);
        Pa_Terminate();
        vosk_recognizer_free(recognizer);
        vosk_model_free(model);
        return 1;
    }
    std::cout << "PortAudio stream started. Using device: " << Pa_GetDeviceInfo(inputParameters.device)->name << std::endl;

    // 7. Start a separate thread to listen for the 'q' command to quit
    std::thread quit_checker_thread(checkForQuitCommand);

    // 8. Main loop: Keep the program running while audio is processed.
    //    The audio processing happens in the PortAudio thread (paCallback).
    //    The main thread waits until g_request_stop is true.
    while (!g_request_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Sleep briefly
    }

    std::cout << "\n'q' pressed. Shutting down..." << std::endl;

    // Ensure the input checker thread finishes
    if (quit_checker_thread.joinable()) {
        quit_checker_thread.join();
    }

    // 9. Stop and Close PortAudio Stream
    pa_err = Pa_StopStream(pa_stream);
    if (pa_err != paNoError) {
        std::cerr << "PortAudio WARNING: Pa_StopStream returned: " << Pa_GetErrorText(pa_err) << std::endl;
    }

    pa_err = Pa_CloseStream(pa_stream);
    if (pa_err != paNoError) {
        std::cerr << "PortAudio WARNING: Pa_CloseStream returned: " << Pa_GetErrorText(pa_err) << std::endl;
    }

    // 10. Terminate PortAudio
    Pa_Terminate();
    std::cout << "PortAudio terminated." << std::endl;

    // 11. Get any final buffered result from Vosk
    const char* final_buffered_result_json = vosk_recognizer_final_result(recognizer);
    if (final_buffered_result_json && strlen(final_buffered_result_json) > 0) {
        // Avoid printing empty final results like {"text" : ""}
        if (std::string(final_buffered_result_json).find("\"text\" : \"\"") == std::string::npos) {
            std::cout << "Final (on exit): " << final_buffered_result_json << std::endl;
        }
    }

    // 12. Clean up Vosk resources
    vosk_recognizer_free(recognizer);
    vosk_model_free(model);
    std::cout << "Vosk resources freed. Exiting." << std::endl;

    return 0;
}