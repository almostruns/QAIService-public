#include "knowledge/document_extractor.h"

#include "util/utf8.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <poppler-document.h>
#include <poppler-page.h>
#include <zip.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace qaiservice::knowledge {
namespace {

constexpr std::size_t kMaximumExpandedBytes = 32 * 1024 * 1024;

std::string readFile(const std::filesystem::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw DocumentExtractionError("document_missing");
  }
  std::string content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  if (content.empty()) {
    throw DocumentExtractionError("document_empty");
  }
  if (content.size() > kMaximumExpandedBytes) {
    throw DocumentExtractionError("document_expansion_too_large");
  }
  return content;
}

ExtractedDocument extractPdf(const std::filesystem::path& path)
{
  std::unique_ptr<poppler::document> document(poppler::document::load_from_file(path.string()));
  if (document == nullptr || document->is_locked()) {
    throw DocumentExtractionError("pdf_unreadable");
  }
  ExtractedDocument extracted;
  for (int index = 0; index < document->pages(); ++index) {
    std::unique_ptr<poppler::page> page(document->create_page(index));
    if (page == nullptr) {
      throw DocumentExtractionError("pdf_page_unreadable");
    }
    const poppler::byte_array utf8 = page->text().to_utf8();
    const std::string text(utf8.begin(), utf8.end());
    if (!text.empty()) {
      extracted.sections.push_back({static_cast<std::uint32_t>(index + 1), text});
    }
  }
  if (extracted.sections.empty()) {
    throw DocumentExtractionError("document_has_no_text");
  }
  return extracted;
}

std::string readDocxXml(const std::filesystem::path& path)
{
  int error = 0;
  std::unique_ptr<zip_t, decltype(&zip_close)> archive(zip_open(path.c_str(), ZIP_RDONLY, &error), zip_close);
  if (archive == nullptr) {
    throw DocumentExtractionError("docx_unreadable");
  }
  zip_stat_t metadata{};
  if (zip_stat(archive.get(), "word/document.xml", ZIP_FL_ENC_GUESS, &metadata) != 0 ||
      metadata.size == 0 || metadata.size > kMaximumExpandedBytes) {
    throw DocumentExtractionError("docx_document_xml_invalid");
  }
  std::unique_ptr<zip_file_t, decltype(&zip_fclose)> file(
      zip_fopen(archive.get(), "word/document.xml", ZIP_FL_ENC_GUESS), zip_fclose);
  if (file == nullptr) {
    throw DocumentExtractionError("docx_document_xml_missing");
  }
  std::string xml(static_cast<std::size_t>(metadata.size), '\0');
  const zip_int64_t read = zip_fread(file.get(), xml.data(), metadata.size);
  if (read != static_cast<zip_int64_t>(metadata.size)) {
    throw DocumentExtractionError("docx_document_xml_unreadable");
  }
  return xml;
}

void collectXmlText(xmlNode* node, std::string& output)
{
  for (xmlNode* current = node; current != nullptr; current = current->next) {
    if (current->type == XML_TEXT_NODE && current->content != nullptr) {
      output += reinterpret_cast<const char*>(current->content);
    }
    if (current->children != nullptr) {
      collectXmlText(current->children, output);
    }
    if (current->type == XML_ELEMENT_NODE && xmlStrEqual(current->name, BAD_CAST "p")) {
      output += "\n\n";
    }
  }
}

ExtractedDocument extractDocx(const std::filesystem::path& path)
{
  const std::string xml = readDocxXml(path);
  std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> document(
      xmlReadMemory(xml.data(), static_cast<int>(xml.size()), "document.xml", nullptr,
                    XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING),
      xmlFreeDoc);
  if (document == nullptr) {
    throw DocumentExtractionError("docx_xml_malformed");
  }
  std::string text;
  collectXmlText(xmlDocGetRootElement(document.get()), text);
  if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
    throw DocumentExtractionError("document_has_no_text");
  }
  return {{{std::nullopt, std::move(text)}}};
}

void appendParagraph(std::vector<NewDocumentChunk>& chunks, std::string& pending, std::string paragraph,
                     std::optional<std::uint32_t> page, std::size_t target, std::size_t hard_limit)
{
  while (paragraph.size() > hard_limit) {
    const std::size_t boundary = util::utf8Boundary(paragraph, hard_limit);
    if (!pending.empty()) {
      chunks.push_back({static_cast<std::uint32_t>(chunks.size()), std::move(pending), page});
      pending.clear();
    }
    chunks.push_back({static_cast<std::uint32_t>(chunks.size()), paragraph.substr(0, boundary), page});
    paragraph.erase(0, boundary);
  }
  const std::size_t separator = pending.empty() ? 0 : 2;
  if (!pending.empty() && pending.size() + separator + paragraph.size() > target) {
    chunks.push_back({static_cast<std::uint32_t>(chunks.size()), std::move(pending), page});
    pending.clear();
  }
  if (!paragraph.empty()) {
    if (!pending.empty()) {
      pending += "\n\n";
    }
    pending += std::move(paragraph);
  }
}

}  // namespace

DocumentExtractionError::DocumentExtractionError(const std::string& message) : std::runtime_error(message)
{
}

ExtractedDocument DocumentExtractor::extract(const std::string& media_type, const std::filesystem::path& path) const
{
  if (media_type == "text/plain" || media_type == "text/markdown") {
    return {{{std::nullopt, readFile(path)}}};
  }
  if (media_type == "application/pdf") {
    return extractPdf(path);
  }
  if (media_type == "application/vnd.openxmlformats-officedocument.wordprocessingml.document") {
    return extractDocx(path);
  }
  throw DocumentExtractionError("unsupported_media_type");
}

std::vector<NewDocumentChunk> chunkDocument(const ExtractedDocument& document, std::size_t target_bytes,
                                            std::size_t hard_limit_bytes)
{
  if (target_bytes == 0 || hard_limit_bytes < target_bytes) {
    throw std::invalid_argument("invalid chunk limits");
  }
  std::vector<NewDocumentChunk> chunks;
  for (const ExtractedSection& section : document.sections) {
    std::string pending;
    std::istringstream lines(section.text);
    std::string line;
    while (std::getline(lines, line)) {
      if (line.empty()) {
        continue;
      }
      appendParagraph(chunks, pending, std::move(line), section.page_number, target_bytes, hard_limit_bytes);
    }
    if (!pending.empty()) {
      chunks.push_back({static_cast<std::uint32_t>(chunks.size()), std::move(pending), section.page_number});
    }
  }
  if (chunks.empty()) {
    throw DocumentExtractionError("document_has_no_text");
  }
  return chunks;
}

}  // namespace qaiservice::knowledge
