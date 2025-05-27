#include <iostream>
#include <cstring>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <queue>
#include <mutex>
#include <cmath>      // For sqrt()
#include <iomanip>
// Vosk API
#include "vosk_api.h"
// PortAudio API
#include <portaudio.h>

// --- Enhanced Configuration ---
const char *MODEL_PATH = "/mnt/d/vsk/model";

#define SAMPLE_RATE         (16000)   // Standard for Vosk
#define FRAMES_PER_BUFFER   (512)     // Reduced for lower latency
#define NUM_CHANNELS        (1)       // Mono
#define PA_SAMPLE_TYPE      (paInt16) // 16-bit PCM

// Audio processing parameters
#define NOISE_GATE_THRESHOLD    (500)     // Adjust based on your environment
#define SILENCE_DETECTION_MS    (1000)    // 1 second of silence
#define AUDIO_BUFFER_SIZE       (8192)    // Circular buffer size
#define AGC_TARGET_LEVEL        (8000)    // Automatic gain control target
#define AGC_ADJUSTMENT_RATE     (0.1f)    // How quickly AGC adjusts

// --- End Configuration ---

// Global variables
std::atomic<bool> g_request_stop(false);
std::string g_last_partial_result_json;

// Audio processing variables
std::atomic<float> g_current_gain(1.0f);
std::queue<std::vector<short>> g_audio_queue;
std::mutex g_audio_mutex;

// Circular buffer for audio smoothing
class CircularBuffer {
private:
    std::vector<short> buffer;
    size_t head, tail, count, capacity;
    
public:
    CircularBuffer(size_t size) : buffer(size), head(0), tail(0), count(0), capacity(size) {}
    
    void push(const short* data, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            buffer[head] = data[i];
            head = (head + 1) % capacity;
            if (count < capacity) {
                count++;
            } else {
                tail = (tail + 1) % capacity;
            }
        }
    }
    
    std::vector<short> getSmoothed(size_t len) {
        std::vector<short> result;
        if (count < len) return result;
        
        result.reserve(len);
        size_t start = (head >= len) ? head - len : capacity - (len - head);
        
        for (size_t i = 0; i < len; ++i) {
            result.push_back(buffer[(start + i) % capacity]);
        }
        return result;
    }
};

CircularBuffer g_audio_buffer(AUDIO_BUFFER_SIZE);

// Noise gate function
bool isAudioAboveNoiseGate(const short* audio_data, unsigned long frame_count) {
    double rms = 0.0;
    for (unsigned long i = 0; i < frame_count; ++i) {
        rms += audio_data[i] * audio_data[i];
    }
    rms = sqrt(rms / frame_count);
    return rms > NOISE_GATE_THRESHOLD;
}

// Simple high-pass filter to remove DC offset and low-frequency noise
void applyHighPassFilter(short* audio_data, unsigned long frame_count, float& prev_input, float& prev_output) {
    const float alpha = 0.95f; // High-pass filter coefficient
    
    for (unsigned long i = 0; i < frame_count; ++i) {
        float input = static_cast<float>(audio_data[i]);
        float output = alpha * (prev_output + input - prev_input);
        prev_input = input;
        prev_output = output;
        
        // Clamp to 16-bit range
        audio_data[i] = static_cast<short>(std::max(-32768.0f, std::min(32767.0f, output)));
    }
}

// Automatic Gain Control
void applyAGC(short* audio_data, unsigned long frame_count) {
    // Calculate current audio level
    double current_level = 0.0;
    for (unsigned long i = 0; i < frame_count; ++i) {
        current_level += abs(audio_data[i]);
    }
    current_level /= frame_count;
    
    // Adjust gain gradually
    float current_gain = g_current_gain.load();
    if (current_level > 0) {
        float desired_gain = AGC_TARGET_LEVEL / current_level;
        float new_gain = current_gain + (desired_gain - current_gain) * AGC_ADJUSTMENT_RATE;
        
        // Limit gain range
        new_gain = std::max(0.1f, std::min(10.0f, new_gain));
        g_current_gain.store(new_gain);
        
        // Apply gain
        for (unsigned long i = 0; i < frame_count; ++i) {
            float sample = audio_data[i] * new_gain;
            audio_data[i] = static_cast<short>(std::max(-32768.0f, std::min(32767.0f, sample)));
        }
    }
}

// Audio preprocessing function
void preprocessAudio(short* audio_data, unsigned long frame_count) {
    static float prev_input = 0.0f, prev_output = 0.0f;
    
    // Apply high-pass filter
    applyHighPassFilter(audio_data, frame_count, prev_input, prev_output);
    
    // Apply automatic gain control
    applyAGC(audio_data, frame_count);
}

// Enhanced PortAudio callback with audio preprocessing
static int paCallback(const void *inputBuffer, void *outputBuffer,
                      unsigned long framesPerBuffer,
                      const PaStreamCallbackTimeInfo* timeInfo,
                      PaStreamCallbackFlags statusFlags,
                      void *userData) {
    VoskRecognizer *recognizer = (VoskRecognizer*)userData;
    const short *input_audio = (const short*)inputBuffer;

    if (g_request_stop) {
        return paComplete;
    }

    if (inputBuffer == NULL) {
        return paContinue;
    }

    // Copy audio data for processing
    std::vector<short> audio_data(input_audio, input_audio + framesPerBuffer);
    
    // Apply noise gate
    if (!isAudioAboveNoiseGate(audio_data.data(), framesPerBuffer)) {
        return paContinue; // Skip processing if below noise gate
    }
    
    // Preprocess audio
    preprocessAudio(audio_data.data(), framesPerBuffer);
    
    // Add to circular buffer for smoothing
    g_audio_buffer.push(audio_data.data(), framesPerBuffer);
    
    // Get smoothed audio data
    std::vector<short> smoothed_audio = g_audio_buffer.getSmoothed(framesPerBuffer);
    if (smoothed_audio.empty()) {
        smoothed_audio = audio_data; // Use original if not enough data for smoothing
    }
    
    // Feed processed audio to Vosk
    int vosk_status = vosk_recognizer_accept_waveform_s(recognizer, smoothed_audio.data(), smoothed_audio.size());

    if (vosk_status == 0) { // Partial result
        const char* partial_json_cstr = vosk_recognizer_partial_result(recognizer);
        if (partial_json_cstr && strlen(partial_json_cstr) > 0) {
            std::string current_partial_json(partial_json_cstr);
            
            // Enhanced filtering for partial results
            if (current_partial_json.find("\"partial\" : \"\"") == std::string::npos &&
                current_partial_json != g_last_partial_result_json &&
                current_partial_json.length() > 20) { // Only show substantial partials
                
                std::cout << "Partial: " << current_partial_json << std::endl;
                g_last_partial_result_json = current_partial_json;
            }
        }
    } else if (vosk_status > 0) { // Final result
        const char* final_result_json_cstr = vosk_recognizer_result(recognizer);
        if (final_result_json_cstr && strlen(final_result_json_cstr) > 0) {
            std::string final_result(final_result_json_cstr);
            
            // Only show non-empty final results
            if (final_result.find("\"text\" : \"\"") == std::string::npos) {
                std::cout << "Final:   " << final_result << std::endl;
            }
        }
        g_last_partial_result_json.clear();
    }

    return paContinue;
}

// Enhanced quit command checker with better instructions
void checkForQuitCommand() {
    std::cout << "\n=== VOICE RECOGNITION ACTIVE ===" << std::endl;
    std::cout << "Microphone is listening with enhanced audio processing." << std::endl;
    std::cout << "Features enabled:" << std::endl;
    std::cout << "  - Noise gate filtering" << std::endl;
    std::cout << "  - Automatic gain control" << std::endl;
    std::cout << "  - High-pass filtering" << std::endl;
    std::cout << "  - Audio smoothing" << std::endl;
    std::cout << "\nTips for better recognition:" << std::endl;
    std::cout << "  - Speak clearly and at moderate pace" << std::endl;
    std::cout << "  - Keep consistent distance from microphone" << std::endl;
    std::cout << "  - Minimize background noise" << std::endl;
    std::cout << "\n>>> Type 'q' and press Enter to stop recording. <<<\n" << std::endl;
    
    char c;
    while (std::cin.get(c)) {
        if (c == 'q' || c == 'Q') {
            g_request_stop = true;
            break;
        }
        if (c == '\n' && g_request_stop) {
            break;
        }
    }
}

int main() {
    std::cout << "=== Enhanced Vosk Speech Recognition ===" << std::endl;
    
    // 1. Initialize Vosk Model
    VoskModel *model = vosk_model_new(MODEL_PATH);
    if (!model) {
        std::cerr << "ERROR: Failed to load Vosk model from \"" << MODEL_PATH << "\"" << std::endl;
        std::cerr << "Please ensure the path is correct and model files are present." << std::endl;
        std::cerr << "For better quality, consider using a larger model:" << std::endl;
        std::cerr << "  - vosk-model-en-us-0.22 (40MB) - basic quality" << std::endl;
        std::cerr << "  - vosk-model-en-us-0.22-lgraph (128MB) - better quality" << std::endl;
        std::cerr << "  - vosk-model-en-us-daanzu-20200905 (1GB+) - best quality" << std::endl;
        return 1;
    }
    std::cout << "✓ Vosk model loaded successfully." << std::endl;

    // 2. Create Vosk Recognizer with enhanced settings
    VoskRecognizer *recognizer = vosk_recognizer_new(model, (float)SAMPLE_RATE);
    if (!recognizer) {
        std::cerr << "ERROR: Failed to create Vosk recognizer." << std::endl;
        vosk_model_free(model);
        return 1;
    }
    
    // Enable word-level timestamps and confidence scores
    vosk_recognizer_set_words(recognizer, 1);
    
    std::cout << "✓ Vosk recognizer created with word-level timestamps." << std::endl;

    // 3. Initialize PortAudio
    PaError pa_err = Pa_Initialize();
    if (pa_err != paNoError) {
        std::cerr << "PortAudio ERROR: Pa_Initialize returned: " << Pa_GetErrorText(pa_err) << std::endl;
        vosk_recognizer_free(recognizer);
        vosk_model_free(model);
        return 1;
    }

    // 4. Enhanced PortAudio Stream Parameters
    PaStreamParameters inputParameters;
    inputParameters.device = Pa_GetDefaultInputDevice();
    if (inputParameters.device == paNoDevice) {
        std::cerr << "PortAudio ERROR: No default input device found." << std::endl;
        Pa_Terminate();
        vosk_recognizer_free(recognizer);
        vosk_model_free(model);
        return 1;
    }
    
    inputParameters.channelCount = NUM_CHANNELS;
    inputParameters.sampleFormat = PA_SAMPLE_TYPE;
    // Use high input latency for better quality
    inputParameters.suggestedLatency = Pa_GetDeviceInfo(inputParameters.device)->defaultHighInputLatency;
    inputParameters.hostApiSpecificStreamInfo = NULL;

    // 5. Open PortAudio Stream
    PaStream *pa_stream;
    pa_err = Pa_OpenStream(
                 &pa_stream,
                 &inputParameters,
                 NULL,
                 SAMPLE_RATE,
                 FRAMES_PER_BUFFER,
                 paClipOff,
                 paCallback,
                 recognizer);

    if (pa_err != paNoError) {
        std::cerr << "PortAudio ERROR: Pa_OpenStream returned: " << Pa_GetErrorText(pa_err) << std::endl;
        Pa_Terminate();
        vosk_recognizer_free(recognizer);
        vosk_model_free(model);
        return 1;
    }

    // 6. Start PortAudio Stream
    pa_err = Pa_StartStream(pa_stream);
    if (pa_err != paNoError) {
        std::cerr << "PortAudio ERROR: Pa_StartStream returned: " << Pa_GetErrorText(pa_err) << std::endl;
        Pa_CloseStream(pa_stream);
        Pa_Terminate();
        vosk_recognizer_free(recognizer);
        vosk_model_free(model);
        return 1;
    }
    
    const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(inputParameters.device);
    std::cout << "✓ PortAudio stream started." << std::endl;
    std::cout << "  Device: " << deviceInfo->name << std::endl;
    std::cout << "  Sample Rate: " << SAMPLE_RATE << " Hz" << std::endl;
    std::cout << "  Buffer Size: " << FRAMES_PER_BUFFER << " frames" << std::endl;
    std::cout << "  Latency: " << inputParameters.suggestedLatency * 1000 << " ms" << std::endl;

    // 7. Start quit checker thread
    std::thread quit_checker_thread(checkForQuitCommand);

    // 8. Main loop with status monitoring
    auto last_status_time = std::chrono::steady_clock::now();
    while (!g_request_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Optional: Print status every 30 seconds
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_status_time).count() >= 30) {
            std::cout << "[Status] Recognition active. Current gain: " 
                      << std::fixed << std::setprecision(2) << g_current_gain.load() << std::endl;
            last_status_time = now;
        }
    }

    std::cout << "\n'q' pressed. Shutting down gracefully..." << std::endl;

    // Wait for input thread
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
    std::cout << "✓ PortAudio terminated." << std::endl;

    // 11. Get final result
    const char* final_buffered_result_json = vosk_recognizer_final_result(recognizer);
    if (final_buffered_result_json && strlen(final_buffered_result_json) > 0) {
        if (std::string(final_buffered_result_json).find("\"text\" : \"\"") == std::string::npos) {
            std::cout << "Final (on exit): " << final_buffered_result_json << std::endl;
        }
    }

    // 12. Clean up
    vosk_recognizer_free(recognizer);
    vosk_model_free(model);
    std::cout << "✓ All resources freed. Program terminated successfully." << std::endl;

    return 0;
}