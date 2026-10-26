#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

struct tts_api_error : std::runtime_error {
    int status;
    tts_api_error(int status, const std::string & message) : std::runtime_error(message), status(status) {}
};

inline bool tts_no_voice(const std::string & id) {
    return id.empty() || id == "default" || id == "none" || id == "no_ref" ||
           id == "no-ref" || id == "null" || id == "text-only";
}

class tts_voice_store {
    using json = nlohmann::json;
    using path = std::filesystem::path;
    path root;
    std::mutex mutex;

    path voice_path(const std::string & id) const {
        validate_id(id);
        path wav = root / (id + ".wav");
        if (std::filesystem::is_symlink(wav) || std::filesystem::is_symlink(wav.string() + ".txt")) {
            throw tts_api_error(400, "Voice files must not be symbolic links");
        }
        for (const path & file : {wav, path(wav.string() + ".txt")}) {
            if (std::filesystem::exists(file) && !std::filesystem::is_regular_file(file)) {
                throw tts_api_error(400, "Voice paths must be regular files");
            }
        }
        return wav;
    }

    static std::string read_file(const path & file) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            throw std::runtime_error("Cannot read voice file");
        }
        std::string data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (in.bad()) {
            throw std::runtime_error("Cannot read voice file");
        }
        return data;
    }

    static void write_file(const path & file, const std::string & data) {
        std::ofstream out(file, std::ios::binary);
        out.write(data.data(), data.size());
        out.close();
        if (!out) {
            throw std::runtime_error("Cannot write voice file");
        }
    }

    static json metadata(const std::string & id, const path & wav) {
        auto modified = std::filesystem::last_write_time(wav);
        auto time = std::chrono::time_point_cast<std::chrono::seconds>(
            modified - decltype(modified)::clock::now() + std::chrono::system_clock::now());
        return {{"id", id}, {"object", "voice_file"}, {"filename", wav.filename().string()},
                {"bytes", std::filesystem::file_size(wav)}, {"created_at", time.time_since_epoch().count()}};
    }

  public:
    explicit tts_voice_store(const path & directory) : root(std::filesystem::absolute(directory)) {}

    static void validate_id(const std::string & id) {
        if (tts_no_voice(id) || id.find_first_not_of(
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos) {
            throw tts_api_error(400, "voice_id must use ASCII letters, numbers, underscores or hyphens, and not be reserved");
        }
    }

    json list() {
        std::lock_guard<std::mutex> lock(mutex);
        json voices = json::array();
        if (std::filesystem::exists(root)) {
            std::vector<std::string> ids;
            for (const auto & entry : std::filesystem::directory_iterator(root)) {
                if (entry.path().extension() == ".wav" && entry.is_regular_file() && !entry.is_symlink()) {
                    const std::string id = entry.path().stem().string();
                    try {
                        voice_path(id);
                        ids.push_back(id);
                    } catch (const tts_api_error &) {
                        continue;
                    }
                }
            }
            std::sort(ids.begin(), ids.end());
            for (const auto & id : ids) {
                voices.push_back({{"id", id}, {"object", "voice"}, {"ref_wav", id + ".wav"},
                                  {"ref_wavs", nullptr}, {"ref_latent", nullptr}, {"ref_latents", nullptr},
                                  {"ref_embed", nullptr}, {"no_ref", false}});
            }
        }
        voices.push_back({{"id", "none"}, {"object", "voice"}, {"ref_wav", nullptr}, {"ref_wavs", nullptr},
                          {"ref_latent", nullptr}, {"ref_latents", nullptr}, {"ref_embed", nullptr}, {"no_ref", true}});
        std::sort(voices.begin(), voices.end(), [](const json & a, const json & b) { return a.at("id") < b.at("id"); });
        return {{"object", "list"}, {"data", voices}};
    }

    json get(const std::string & id) {
        std::lock_guard<std::mutex> lock(mutex);
        path wav = voice_path(id);
        if (!std::filesystem::is_regular_file(wav)) {
            throw tts_api_error(404, "Voice not found: " + id);
        }
        return metadata(id, wav);
    }

    void load(const std::string & id, std::string & audio, std::string & text, bool load_text) {
        std::lock_guard<std::mutex> lock(mutex);
        path wav = voice_path(id);
        if (!std::filesystem::is_regular_file(wav)) {
            throw tts_api_error(404, "Voice not found: " + id);
        }
        audio = read_file(wav);
        if (audio.empty()) {
            throw tts_api_error(400, "Reference audio is empty");
        }
        if (load_text && std::filesystem::exists(wav.string() + ".txt")) {
            text = read_file(wav.string() + ".txt");
            const auto first = text.find_first_not_of(" \t\r\n");
            text = first == std::string::npos ? "" : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
        }
    }

    json put(const std::string & id, const std::string & audio, const std::string & text, bool replace) {
        std::lock_guard<std::mutex> lock(mutex);
        path wav = voice_path(id);
        bool exists = std::filesystem::exists(wav);
        if (exists && !replace) {
            throw tts_api_error(409, "Voice already exists; use PUT to replace it");
        }
        if (!exists && replace) {
            throw tts_api_error(404, "Voice not found: " + id);
        }
        std::filesystem::create_directories(root);
        path staging = root / (".upload-" + std::to_string(std::random_device{}()));
        if (!std::filesystem::create_directory(staging)) {
            throw std::runtime_error("Cannot stage voice upload");
        }
        path txt = wav.string() + ".txt";
        bool saved_wav = false, saved_txt = false, installed_wav = false, installed_txt = false;
        try {
            write_file(staging / "new.wav", audio);
            if (!text.empty()) {
                write_file(staging / "new.txt", text);
            }
            // Stage both files before replacing either; keep the old pair for rollback on I/O errors.
            if (exists) {
                std::filesystem::rename(wav, staging / "old.wav");
                saved_wav = true;
            }
            if (std::filesystem::exists(txt)) {
                std::filesystem::rename(txt, staging / "old.txt");
                saved_txt = true;
            }
            std::filesystem::rename(staging / "new.wav", wav);
            installed_wav = true;
            if (!text.empty()) {
                std::filesystem::rename(staging / "new.txt", txt);
                installed_txt = true;
            }
        } catch (...) {
            if (installed_wav) { std::filesystem::remove(wav); }
            if (installed_txt) { std::filesystem::remove(txt); }
            if (saved_wav) { std::filesystem::rename(staging / "old.wav", wav); }
            if (saved_txt) { std::filesystem::rename(staging / "old.txt", txt); }
            std::filesystem::remove_all(staging);
            throw;
        }
        std::filesystem::remove_all(staging);
        return metadata(id, wav);
    }

    json remove(const std::string & id) {
        std::lock_guard<std::mutex> lock(mutex);
        path wav = voice_path(id);
        if (!std::filesystem::is_regular_file(wav)) {
            throw tts_api_error(404, "Voice not found: " + id);
        }
        std::filesystem::remove(wav.string() + ".txt");
        std::filesystem::remove(wav);
        return {{"id", id}, {"object", "voice_file"}, {"deleted", true}};
    }
};

struct tts_speech_request {
    nlohmann::json params;
    std::string input;
    std::string reference_wav;
    std::string reference_base64;
    std::string reference_text;
};

inline tts_speech_request tts_resolve_request(nlohmann::json body, tts_voice_store & voices) {
    using json = nlohmann::json;
    if (!body.is_object() || !body.contains("input") || !body.at("input").is_string() ||
        body.at("input").get<std::string>().empty()) {
        throw tts_api_error(400, "input must be a nonempty string");
    }
    json options = body.value("voxcpm", json::object());
    if (!options.is_object()) {
        throw tts_api_error(400, "voxcpm must be an object");
    }
    std::string model = body.value("model", std::string());
    if (!model.empty() && model != "voxcpm" && model != "voxcpm2") {
        throw tts_api_error(400, "Expected model voxcpm2 or voxcpm");
    }
    std::string format = body.value("response_format", std::string("wav"));
    if (format != "wav" && format != "pcm") {
        throw tts_api_error(400, "Supported response_format: wav, pcm");
    }
    for (const char * key : {"seed", "cfg_value", "inference_timesteps", "max_steps", "temperature"}) {
        if (options.contains(key)) {
            body[key] = options.at(key);
        }
        if (body.contains(key)) {
            const auto & value = body.at(key);
            bool integer = std::string(key) == "seed" || std::string(key) == "inference_timesteps" ||
                           std::string(key) == "max_steps";
            double max = integer ? (std::string(key) == "seed" ? double(std::numeric_limits<uint32_t>::max()) :
                                    double(std::numeric_limits<int>::max())) : double(std::numeric_limits<float>::max());
            double min = std::string(key) == "seed" || std::string(key) == "cfg_value" ? 0.0 : (integer ? 1.0 : 0.0);
            if (!value.is_number() || (integer && !value.is_number_integer()) || !std::isfinite(value.get<double>()) ||
                value.get<double>() < min || value.get<double>() > max ||
                (std::string(key) == "temperature" && value.get<double>() == 0)) {
                throw tts_api_error(400, std::string("Invalid ") + key);
            }
        }
    }
    tts_speech_request request;
    request.params = std::move(body);
    request.input = request.params.at("input").get<std::string>();
    std::string caption = options.value("caption", std::string());
    if (!caption.empty()) {
        request.input = "(" + caption + ")" + request.input;
    }
    bool no_audio = options.value("no_reference_audio", false);
    bool no_text = options.value("no_reference_text", false);
    if (no_audio) {
        return request;
    }
    std::string id = options.value("reference_audio", std::string());
    request.reference_base64 = request.params.value("reference_audio", std::string());
    if (!id.empty() && !request.reference_base64.empty()) {
        throw tts_api_error(400, "Do not combine voxcpm.reference_audio with base64 reference_audio");
    }
    if (id.empty() && request.reference_base64.empty()) {
        json voice = request.params.value("voice", json("default"));
        id = voice.is_object() ? voice.at("id").get<std::string>() : voice.get<std::string>();
    }
    bool explicit_text = !no_text && options.contains("reference_text");
    if (!tts_no_voice(id)) {
        voices.load(id, request.reference_wav, request.reference_text, !no_text && !explicit_text);
    }
    if (!no_text) {
        if (explicit_text) {
            request.reference_text = options.at("reference_text").get<std::string>();
        } else {
            for (const char * key : {"prompt_text", "reference_text", "ref_text"}) {
                if (request.params.contains(key) && !request.params.at(key).get<std::string>().empty()) {
                    request.reference_text = request.params.at(key).get<std::string>();
                    break;
                }
            }
        }
    }
    if (!request.reference_text.empty() && request.reference_wav.empty() && request.reference_base64.empty()) {
        throw tts_api_error(400, "reference_text requires reference audio");
    }
    return request;
}
