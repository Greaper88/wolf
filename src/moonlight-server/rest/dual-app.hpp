#pragma once

#include <rest/rest.hpp>

namespace wolf::app_picker {

enum class Operation {
  state,
  catalog,
  assign,
  clear
};

struct Profile {
  std::string id;
  std::string name;
  bool pin_required;
};
struct Display {
  int display_id;
  int width;
  int height;
  int fps;
  std::string state;
  std::string assignment_id;
  std::optional<std::string> app_id;
  std::optional<std::string> app_name;
  std::optional<std::string> profile_id;
};
struct State {
  std::string session_token;
  bool available;
  std::string mode;
  std::vector<Profile> profiles;
  std::vector<Display> displays;
};
struct App {
  std::string id;
  std::string name;
  bool running;
  std::optional<int> assigned_display;
  bool available;
};
struct Response {
  bool success;
  int version = 1;
  std::optional<State> state;
  std::optional<std::vector<App>> apps;
  std::optional<std::string> code;
  std::optional<std::string> error;
};
struct Reply {
  int status;
  Response body;
};
struct CatalogRequest {
  int version;
  std::string session_token;
  std::string profile_id;
  std::optional<std::vector<short>> profile_pin;
};
struct AssignRequest {
  int version;
  std::string session_token;
  int display_id;
  std::string expected_assignment;
  std::string profile_id;
  std::string app_id;
  std::optional<std::vector<short>> profile_pin;
  std::optional<std::vector<short>> lobby_pin;
};
struct ClearRequest {
  int version;
  std::string session_token;
  int display_id;
  std::string expected_assignment;
};

// The HTTPS adapter supplies only a certificate-authenticated paired client.
// Session IDs, runners and paths never come from the remote request.
Reply handle(Operation operation,
             const wolf::config::PairedClient &client,
             const immer::box<state::AppState> &state,
             const std::string &body = {});
void send(const std::shared_ptr<HttpsServer::Response> &response, const Reply &reply);

} // namespace wolf::app_picker
