#pragma once

#include "../domain/Message.h"

#include <cstddef>
#include <string>
#include <vector>


struct message_parse_diagnostics {
  std::string strategy;
  std::size_t response_size = 0;
  std::size_t extracted_size = 0;
  std::size_t headers_count  = 0;
  std::size_t body_size      = 0;
  bool literal_found   = false;
  bool literal_size_ok = false;
  std::string response_prefix;
};


std::string imap_extract_literal(const std::string& response);


std::string strip_imap_framing(const std::string& response);


bool looks_like_email_headers(const std::string& s, std::size_t offset = 0);


std::string imap_extract_raw(const std::string& imap_response,
                              message_parse_diagnostics& diag);


bool is_incomplete_literal(const std::string& response);


void split_headers_body(const std::string& raw,
                        std::vector<std::string>& headers,
                        std::string& body);

std::string get_header(const std::vector<std::string>& headers,
                       const std::string& name);

std::string decode_mime_header(const std::string& value);

std::string extract_part_by_content_type(const std::string& raw,
                                          const std::string& content_type);

std::string snippet_from_body(const std::string& body,
                               std::size_t max_len = 200);

std::vector<message_link> extract_links(const std::string& text,
                                         const std::string& html = "");

std::vector<attachment> extract_attachment_metadata(const std::string& raw);


std::string strip_html_tags(const std::string& html);
