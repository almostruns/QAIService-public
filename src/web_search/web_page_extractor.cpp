#include "web_search/web_page_extractor.h"

#include "util/utf8.h"

#include <libxml/HTMLparser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <set>
#include <string>

namespace qaiservice::web_search {
namespace {

struct DocumentDeleter {
  void operator()(xmlDoc* document) const
  {
    xmlFreeDoc(document);
  }
};

struct XmlStringDeleter {
  void operator()(xmlChar* value) const
  {
    xmlFree(value);
  }
};

std::string normalizeWhitespace(const std::string& text)
{
  std::string normalized;
  normalized.reserve(text.size());
  bool pending_space = false;
  for (unsigned char character : text) {
    if (std::isspace(character) != 0) {
      pending_space = !normalized.empty();
      continue;
    }
    if (pending_space) {
      normalized.push_back(' ');
      pending_space = false;
    }
    normalized.push_back(static_cast<char>(character));
  }
  return normalized;
}

bool named(xmlNode* node, const char* name)
{
  return node->type == XML_ELEMENT_NODE && xmlStrcasecmp(node->name, BAD_CAST name) == 0;
}

bool hidden(xmlNode* node)
{
  if (xmlHasProp(node, BAD_CAST "hidden") != nullptr) {
    return true;
  }
  std::unique_ptr<xmlChar, XmlStringDeleter> aria(xmlGetProp(node, BAD_CAST "aria-hidden"));
  if (aria != nullptr && xmlStrcasecmp(aria.get(), BAD_CAST "true") == 0) {
    return true;
  }
  std::unique_ptr<xmlChar, XmlStringDeleter> style(xmlGetProp(node, BAD_CAST "style"));
  if (style == nullptr) {
    return false;
  }
  std::string normalized = reinterpret_cast<const char*>(style.get());
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return normalized.find("display:none") != std::string::npos || normalized.find("visibility:hidden") != std::string::npos;
}

bool noisy(xmlNode* node)
{
  static const std::set<std::string> names = {
      "script", "style", "nav", "footer", "form", "noscript", "svg", "canvas", "iframe", "aside"};
  if (node->type != XML_ELEMENT_NODE) {
    return false;
  }
  std::string name = reinterpret_cast<const char*>(node->name);
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  if (names.count(name) != 0 || hidden(node)) {
    return true;
  }
  std::unique_ptr<xmlChar, XmlStringDeleter> class_name(xmlGetProp(node, BAD_CAST "class"));
  if (class_name == nullptr) {
    return false;
  }
  std::string classes = reinterpret_cast<const char*>(class_name.get());
  std::transform(classes.begin(), classes.end(), classes.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return classes.find("advert") != std::string::npos || classes.find(" ad-") != std::string::npos ||
         classes.rfind("ad-", 0) == 0;
}

void removeNoise(xmlNode* parent)
{
  for (xmlNode* node = parent->children; node != nullptr;) {
    xmlNode* next = node->next;
    if (noisy(node)) {
      xmlUnlinkNode(node);
      xmlFreeNode(node);
    } else {
      removeNoise(node);
    }
    node = next;
  }
}

xmlNode* findFirst(xmlNode* node, const char* name)
{
  for (xmlNode* current = node; current != nullptr; current = current->next) {
    if (named(current, name)) {
      return current;
    }
    xmlNode* nested = findFirst(current->children, name);
    if (nested != nullptr) {
      return nested;
    }
  }
  return nullptr;
}

std::string nodeText(xmlNode* node)
{
  if (node == nullptr) {
    return {};
  }
  std::unique_ptr<xmlChar, XmlStringDeleter> content(xmlNodeGetContent(node));
  return content == nullptr ? "" : normalizeWhitespace(reinterpret_cast<const char*>(content.get()));
}

std::string truncateUtf8(std::string text, std::size_t maximum_bytes)
{
  if (text.size() <= maximum_bytes) {
    return text;
  }
  text.resize(util::utf8Boundary(text, maximum_bytes));
  return text;
}

}  // namespace

WebPageExtractor::WebPageExtractor(std::size_t maximum_text_bytes) : maximum_text_bytes_(maximum_text_bytes)
{
}

std::optional<ExtractedWebPage> WebPageExtractor::extract(const std::string& body,
                                                          const std::string& content_type) const
{
  if (body.empty() || maximum_text_bytes_ == 0) {
    return std::nullopt;
  }
  if (content_type.rfind("text/plain", 0) == 0) {
    const std::string text = truncateUtf8(normalizeWhitespace(body), maximum_text_bytes_);
    return text.empty() ? std::nullopt : std::optional<ExtractedWebPage>(ExtractedWebPage{"", text});
  }

  const int options = HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING | HTML_PARSE_NONET;
  std::unique_ptr<xmlDoc, DocumentDeleter> document(
      htmlReadMemory(body.data(), static_cast<int>(body.size()), nullptr, "UTF-8", options));
  if (document == nullptr) {
    return std::nullopt;
  }
  xmlNode* root = xmlDocGetRootElement(document.get());
  if (root == nullptr) {
    return std::nullopt;
  }
  const std::string title = nodeText(findFirst(root, "title"));
  removeNoise(root);
  xmlNode* content = findFirst(root, "article");
  if (content == nullptr) {
    content = findFirst(root, "main");
  }
  if (content == nullptr) {
    content = findFirst(root, "body");
  }
  const std::string text = truncateUtf8(nodeText(content), maximum_text_bytes_);
  if (text.empty()) {
    return std::nullopt;
  }
  return ExtractedWebPage{title, text};
}

}  // namespace qaiservice::web_search
