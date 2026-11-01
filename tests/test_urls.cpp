// Offline HTTP tests: verify the actual request target, not just URL strings.
#include "myagent/agent.hpp"
#include "httplib.h"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace myagent;

int main() {
  httplib::Server server;
  server.Post(R"(/.*)", [](const httplib::Request& req, httplib::Response& res) {
    if (req.path == "/v1/audio/transcriptions" ||
        req.path == "/gateway/v1/audio/transcriptions") {
      res.set_content(R"({"text":"Offline transcription"})", "application/json");
    } else if (req.path == "/v1/audio/speech" ||
               req.path == "/gateway/v1/audio/speech") {
      res.set_content("fake-mp3-data", "audio/mpeg");
    } else {
      res.set_content(mymcp::json::serialize(mymcp::json::object{
        {"target", req.target},
        {"authorization", req.get_header_value("Authorization")},
        {"content_type", req.get_header_value("Content-Type")}
      }), "application/json");
    }
  });
  const int port = server.bind_to_any_port("127.0.0.1");
  assert(port > 0);
  std::thread server_thread([&] { server.listen_after_bind(); });
  for (int i = 0; i < 200 && !server.is_running(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  assert(server.is_running());

  const std::string origin = "http://127.0.0.1:" + std::to_string(port);
  auto check = [&](const std::string& base, bool append,
                   const std::string& expected_path) {
    Config config;
    config.base_url = base;
    config.api_key = "sample-offline-key";
    config.append_responses_path = append;
    OpenAIResponses model(config);
    const auto response = model.create(mymcp::json::object{
      {"model", "demo"}, {"input", "test"}
    });
    assert(response.at("target").as_string() == expected_path);
    assert(response.at("authorization").as_string() == "Bearer sample-offline-key");
    assert(response.at("content_type").as_string() == "application/json");
  };

  // Mode 1: auto append a suffix and avoid duplicates.
  check(origin, true, "/v1/responses");
  check(origin + "/", true, "/v1/responses");
  check(origin + "/gateway", true, "/gateway/v1/responses");
  check(origin + "/gateway/?api-version=4", true,
        "/gateway/v1/responses?api-version=4");
  check(origin + "/v1/responses", true, "/v1/responses");
  check(origin + "/v1/responses/", true, "/v1/responses");

  // Mode 2: send to exactly the specified URL, including query string.
  check(origin + "/custom/responses", false, "/custom/responses");
  check(origin + "/gateway/openai/responses?version=2", false,
        "/gateway/openai/responses?version=2");
  check(origin + "/v1/responses", false, "/v1/responses");
  check(origin + "/", false, "/");

  // Audio URLs remain distinct from the model's custom Responses route.
  Config audio_config;
  audio_config.base_url = origin + "/custom/responses?version=2";
  audio_config.api_key = "sample-offline-key";
  audio_config.append_responses_path = false;
  OpenAIResponses audio_model(audio_config);
  const auto temp_dir = fs::temp_directory_path() / "myagent_url_test";
  fs::create_directories(temp_dir);
  const auto input_file = temp_dir / "input.wav";
  {
    std::ofstream out(input_file, std::ios::binary);
    out << "fake audio bytes";
  }
  assert(audio_model.transcribe(input_file) == "Offline transcription");
  const auto output_file = temp_dir / "output.mp3";
  audio_model.speech("Hello!", output_file);
  {
    std::ifstream in(output_file, std::ios::binary);
    const std::string audio((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    assert(audio == "fake-mp3-data");
  }
  // A gateway may route audio under a different prefix/origin. Existing
  // base_url-only clients keep the original /v1/audio behavior.
  audio_config.audio_base_url=origin+"/gateway";
  OpenAIResponses routed_audio(audio_config);
  assert(routed_audio.transcribe(input_file)=="Offline transcription");
  routed_audio.speech("Hello routed!",output_file);
  fs::remove_all(temp_dir);

  // Reject fragments and userinfo URLs to avoid ambiguous routing.
  Config bad;
  bad.api_key = "sample-offline-key";
  bad.base_url = origin + "/endpoint#fragment";
  try { OpenAIResponses invalid(bad); assert(false); }
  catch (const std::invalid_argument&) {}
  bad.base_url = "http://user:password@127.0.0.1/";
  try { OpenAIResponses invalid(bad); assert(false); }
  catch (const std::invalid_argument&) {}

  server.stop();
  server_thread.join();
  std::cout << "Responses URL mode tests OK\n";
}
