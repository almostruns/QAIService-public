#include "http/web_app.h"

#include "http/router.h"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

namespace qaiservice::http {
namespace {

std::string readAsset(const std::filesystem::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot load web asset: " + path.string());
  }
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void addAsset(Router& router, std::string path, std::string content_type, std::string content)
{
  router.add("GET", std::move(path), [content_type = std::move(content_type), content = std::move(content)](
                                         Request, ResponseWriter writer) {
    Response response{200, content_type, content};
    response.headers["Cache-Control"] = "no-store";
    writer.send(std::move(response));
  });
}

void addProtectedPage(Router& router, std::string path, std::string content, PageAccessCheck authenticated)
{
  router.add("GET", std::move(path), [content = std::move(content), authenticated = std::move(authenticated)](
                                         Request request, ResponseWriter writer) {
    if (!authenticated || !authenticated(request)) {
      writer.send({401, "application/json", R"({"error":"authentication_required"})",
                   {{"Cache-Control", "no-store"}}});
      return;
    }
    writer.send({200, "text/html; charset=utf-8", content, {{"Cache-Control", "no-store"}}});
  });
}

void addProtectedRedirect(Router& router, std::string path, std::string target, PageAccessCheck authenticated)
{
  router.add("GET", std::move(path), [target = std::move(target), authenticated = std::move(authenticated)](
                                         Request request, ResponseWriter writer) {
    if (!authenticated || !authenticated(request)) {
      writer.send({401, "application/json", R"({"error":"authentication_required"})",
                   {{"Cache-Control", "no-store"}}});
      return;
    }
    writer.send({302, "text/plain; charset=utf-8", "", {{"Cache-Control", "no-store"}, {"Location", target}}});
  });
}

void addRegistrationPage(Router& router, std::string content, bool registration_enabled)
{
  router.add("GET", "/register", [content = std::move(content), registration_enabled](Request,
                                                                                      ResponseWriter writer) {
    if (!registration_enabled) {
      writer.send({403, "application/json", R"({"error":"registration_disabled"})",
                   {{"Cache-Control", "no-store"}}});
      return;
    }
    writer.send({200, "text/html; charset=utf-8", content, {{"Cache-Control", "no-store"}}});
  });
}

}  // namespace

void registerWebAppRoutes(Router& router, const std::filesystem::path& web_root, WebAppConfig config)
{
  addAsset(router, "/", "text/html; charset=utf-8", readAsset(web_root / "pages/landing.html"));
  addAsset(router, "/login", "text/html; charset=utf-8", readAsset(web_root / "pages/login.html"));
  addRegistrationPage(router, readAsset(web_root / "pages/register.html"), config.registration_enabled);
  addProtectedRedirect(router, "/account", "/app", config.authenticated);
  addProtectedPage(router, "/chat", readAsset(web_root / "pages/chat.html"), config.authenticated);
  addProtectedPage(router, "/app", readAsset(web_root / "pages/assistant.html"), config.authenticated);

  addAsset(router, "/assets/css/tokens.css", "text/css; charset=utf-8",
           readAsset(web_root / "assets/css/tokens.css"));
  addAsset(router, "/assets/css/base.css", "text/css; charset=utf-8", readAsset(web_root / "assets/css/base.css"));
  addAsset(router, "/assets/css/components.css", "text/css; charset=utf-8",
           readAsset(web_root / "assets/css/components.css"));
  addAsset(router, "/assets/css/auth.css", "text/css; charset=utf-8", readAsset(web_root / "assets/css/auth.css"));
  addAsset(router, "/assets/css/chat.css", "text/css; charset=utf-8", readAsset(web_root / "assets/css/chat.css"));
  addAsset(router, "/assets/css/assistant.css", "text/css; charset=utf-8",
           readAsset(web_root / "assets/css/assistant.css"));
  addAsset(router, "/assets/css/focus_timer.css", "text/css; charset=utf-8",
           readAsset(web_root / "assets/css/focus_timer.css"));
  addAsset(router, "/assets/js/api.js", "text/javascript; charset=utf-8", readAsset(web_root / "assets/js/api.js"));
  addAsset(router, "/assets/js/request_id.js", "text/javascript; charset=utf-8",
           readAsset(web_root / "assets/js/request_id.js"));
  addAsset(router, "/assets/js/chat_progress.js", "text/javascript; charset=utf-8",
           readAsset(web_root / "assets/js/chat_progress.js"));
  addAsset(router, "/assets/js/chat_request.js", "text/javascript; charset=utf-8",
           readAsset(web_root / "assets/js/chat_request.js"));
  addAsset(router, "/assets/js/chart_palette.js", "text/javascript; charset=utf-8",
           readAsset(web_root / "assets/js/chart_palette.js"));
  addAsset(router, "/assets/js/focus_timer.js", "text/javascript; charset=utf-8",
           readAsset(web_root / "assets/js/focus_timer.js"));
  addAsset(router, "/assets/js/auth.js", "text/javascript; charset=utf-8",
           readAsset(web_root / "assets/js/auth.js"));
  addAsset(router, "/assets/js/chat.js", "text/javascript; charset=utf-8",
           readAsset(web_root / "assets/js/chat.js"));
  addAsset(router, "/assets/js/assistant.js", "text/javascript; charset=utf-8",
           readAsset(web_root / "assets/js/assistant.js"));
}

}  // namespace qaiservice::http
