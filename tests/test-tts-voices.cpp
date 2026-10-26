#ifdef NDEBUG
#undef NDEBUG
#endif

#include "../tools/server/server-tts-voices.h"

#include <cassert>
#include <cstdio>
#include <functional>

using json = nlohmann::json;

static void test_expect_error(int status, int line, const std::function<void()> & action) {
    try {
        action();
        std::fprintf(stderr, "Expected HTTP %d at test line %d\n", status, line);
        assert(false);
    } catch (const tts_api_error & error) {
        assert(error.status == status);
    }
}

#define expect_error(status, ...) test_expect_error(status, __LINE__, __VA_ARGS__)

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("tts-voices-test-" + std::to_string(std::random_device{}()));
    tts_voice_store voices(root);
    assert(voices.list().at("data").size() == 1);
    assert(!fs::exists(root));
    expect_error(400, [&] { voices.get("../escape"); });
    expect_error(400, [&] { voices.get("none"); });
    expect_error(404, [&] { voices.get("missing"); });
    expect_error(404, [&] { voices.put("missing", "audio", "", true); });

    auto created = voices.put("ref_123", "audio", "Hello!\n", false);
    assert(created.at("object") == "voice_file");
    assert(created.at("filename") == "ref_123.wav");
    assert(created.at("bytes") == 5);
    assert(created.at("created_at").get<int64_t>() > 0);
    auto listed = voices.list().at("data");
    assert(listed.at(0).at("id") == "none");
    assert(listed.at(0).at("ref_wav").is_null());
    assert(listed.at(1).at("ref_wav") == "ref_123.wav");
    expect_error(409, [&] { voices.put("ref_123", "other", "", false); });

    json body = {{"input", "Test."}, {"model", "voxcpm2"}, {"voice", "ref_123"}, {"seed", 42},
                 {"voxcpm", {{"seed", 12345}, {"caption", "calm"}}}};
    auto request = tts_resolve_request(body, voices);
    assert(request.input == "(calm)Test.");
    assert(request.params.at("seed") == 12345);
    assert(request.reference_wav == "audio");
    assert(request.reference_text == "Hello!");
    auto tuned = body;
    tuned["voxcpm"].update({{"cfg_value", 2.5}, {"inference_timesteps", 3},
                            {"max_steps", 80}, {"temperature", 0.9}});
    auto tuned_request = tts_resolve_request(tuned, voices);
    assert(tuned_request.params.at("cfg_value") == 2.5);
    assert(tuned_request.params.at("inference_timesteps") == 3);
    assert(tuned_request.params.at("max_steps") == 80);
    assert(tuned_request.params.at("temperature") == 0.9);
    body["voxcpm"]["reference_text"] = "Override";
    assert(tts_resolve_request(body, voices).reference_text == "Override");
    body["voxcpm"]["reference_text"] = "";
    assert(tts_resolve_request(body, voices).reference_text.empty());
    body["voxcpm"]["reference_text"] = "Ignored";
    body["voxcpm"]["no_reference_text"] = true;
    assert(tts_resolve_request(body, voices).reference_text.empty());
    body["voxcpm"].erase("reference_text");
    body["voxcpm"]["no_reference_text"] = false;
    for (bool no_audio : {false, true}) {
        for (bool no_text : {false, true}) {
            body["voxcpm"]["no_reference_audio"] = no_audio;
            body["voxcpm"]["no_reference_text"] = no_text;
            auto resolved = tts_resolve_request(body, voices);
            assert(resolved.reference_wav.empty() == no_audio);
            assert(resolved.reference_text.empty() == (no_audio || no_text));
        }
    }
    body["voice"] = "missing";
    assert(tts_resolve_request(body, voices).reference_wav.empty());
    body["voxcpm"] = {{"reference_audio", "ref_123"}};
    assert(tts_resolve_request(body, voices).reference_wav == "audio");
    body["voice"] = "none";
    assert(tts_resolve_request(body, voices).reference_wav == "audio");
    body["reference_audio"] = "base64";
    expect_error(400, [&] { tts_resolve_request(body, voices); });
    body["voxcpm"] = json::object();
    body["prompt_text"] = "Legacy";
    assert(tts_resolve_request(body, voices).reference_base64 == "base64");
    assert(tts_resolve_request(body, voices).reference_text == "Legacy");
    body.erase("reference_audio");
    expect_error(400, [&] { tts_resolve_request(body, voices); });
    body.erase("prompt_text");
    body["voice"] = {{"id", "ref_123"}};
    assert(tts_resolve_request(body, voices).reference_text == "Hello!");

    for (json options : {json{{"seed", -1}}, json{{"seed", 4294967296ULL}},
                         json{{"inference_timesteps", 0}}, json{{"inference_timesteps", 1.5}},
                         json{{"max_steps", "10"}}, json{{"temperature", 0}}, json{{"cfg_value", -1}}}) {
        body["voxcpm"] = options;
        expect_error(400, [&] { tts_resolve_request(body, voices); });
    }
    body["voxcpm"] = {{"seed", 4294967295ULL}};
    assert(tts_resolve_request(body, voices).params.at("seed") == 4294967295ULL);

    voices.put("ref_123", "replacement", "", true);
    assert(!fs::exists(root / "ref_123.wav.txt"));
    assert(tts_resolve_request(body, voices).reference_text.empty());
    tts_voice_store reopened(root);
    assert(reopened.get("ref_123").at("bytes") == 11);
    std::ofstream(root / "ref_123.wav.txt") << "Manual transcript";
    assert(tts_resolve_request(body, reopened).reference_text == "Manual transcript");
    voices.put("ref_456", "second", "Second transcript", false);
#ifndef _WIN32
    fs::create_symlink(root / "ref_456.wav", root / "linked.wav");
    expect_error(400, [&] { voices.get("linked"); });
    fs::create_symlink(root / "ref_456.wav.txt", root / "ref_123.wav.txt.link");
    fs::remove(root / "ref_123.wav.txt");
    fs::rename(root / "ref_123.wav.txt.link", root / "ref_123.wav.txt");
    expect_error(400, [&] { voices.remove("ref_123"); });
    fs::remove(root / "ref_123.wav.txt");
#endif
    assert(voices.remove("ref_123").at("deleted") == true);
    expect_error(404, [&] { voices.remove("ref_123"); });
    assert(voices.get("ref_456").at("bytes") == 6);
    fs::remove_all(root);
    std::puts("tts voice storage and request resolution: passed");
}
