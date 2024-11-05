#include <array>
#include <cstdlib>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <git2.h>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>

#include "serialise.hh"
#include "processes.hh"


namespace fs = std::filesystem;

namespace nix {
namespace lfs {

// see Fetch::rules
struct AttrRule {
    std::string pattern;
    std::map<std::string, std::string> attributes;
};

// git-lfs metadata about a file
struct Md {
  std::string path; // fs path relative to repo root, no ./ prefix
  std::string oid; // git-lfs managed object id. you give this to the lfs server
                   // for downloads
  size_t size;     // in bytes
};

struct Fetch {
  // only true after init()
  bool ready = false;

  // from shelling out to ssh, used  for 2 subsequent fetches:
  // list of URLs to fetch from, and fetching the data itself
  std::string token = "";

  // this is the URL you hit to get another list of URLs for subsequent fetches
  // e.g. https://github.com/owner/repo.git/info/lfs/objects/batch
  // determined from the git remote
  std::string rootUrl = "";

  // parsed contents of .gitattributes
  // .gitattributes contains a list of path patterns, and list of attributes (=key-value tags) for each pattern
  // paths tagged with `filter=lfs` need to be smudged by downloading from lfs server
  std::vector<AttrRule> rules = {};

  void init(git_repository* repo, std::string gitattributesContent);
  bool hasAttribute(const std::string& path, const std::string& attrName) const;
  void fetch(const std::string& pointerFileContents, const std::string& pointerFilePath, Sink& sink) const;
  std::vector<nlohmann::json> fetchUrls(const std::vector<Md> &metadatas) const;
};

bool matchesPattern(std::string_view path, std::string_view pattern) {
    if (pattern.ends_with("/**")) {
        auto prefix = pattern.substr(0, pattern.length() - 3);
        return path.starts_with(prefix);
    }
    size_t patternPos = 0;
    size_t pathPos = 0;

    while (patternPos < pattern.length() && pathPos < path.length()) {
        if (pattern[patternPos] == '*') {
            // For "*.ext" pattern, match against end of path
            if (patternPos == 0 && pattern.find('*', 1) == std::string_view::npos) {
                return path.ends_with(pattern.substr(1));
            }
            auto nextPatternChar = pattern[patternPos + 1];
            while (pathPos < path.length() && path[pathPos] != nextPatternChar) {
                pathPos++;
            }
            patternPos++;
        } else if (pattern[patternPos] == path[pathPos]) {
            patternPos++;
            pathPos++;
        } else {
            return false;
        }
    }

    return patternPos == pattern.length() && pathPos == path.length();
}

static size_t writeCallback(void *contents, size_t size, size_t nmemb,
                            std::string *s) {
  size_t newLength = size * nmemb;
  try {
    s->append((char *)contents, newLength);
  } catch (std::bad_alloc &e) {
    // Handle memory bad_alloc error
    return 0;
  }
  return newLength;
}

struct SinkCallbackData {
    Sink* sink;
    std::string_view sha256Expected;
    HashSink hashSink;
    
    SinkCallbackData(Sink* sink, std::string_view sha256) 
        : sink(sink)
        , sha256Expected(sha256)
        , hashSink(HashAlgorithm::SHA256) 
    {}
};

static size_t sinkWriteCallback(void *contents, size_t size, size_t nmemb, SinkCallbackData *data) {
    size_t totalSize = size * nmemb;
    try {
        data->hashSink({(char *)contents, totalSize});
        (*data->sink)({(char *)contents, totalSize});
    } catch (std::exception &e) {
        return 0;
    }
    return totalSize;
}

void downloadToSink(const std::string &url, const std::string &authHeader, Sink &sink, std::string_view sha256Expected) {
    CURL *curl;
    CURLcode res;

    curl = curl_easy_init();
    if (curl) {
        SinkCallbackData data(&sink, sha256Expected);
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sinkWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        struct curl_slist *headers = nullptr;
        const std::string authHeader_prepend = "Authorization: " + authHeader;
        headers = curl_slist_append(headers, authHeader_prepend.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            throw std::runtime_error(std::string("curl_easy_perform() failed: ") + curl_easy_strerror(res));
        }

        const auto sha256Actual = data.hashSink.finish().first.to_string(HashFormat::Base16, false);
        if (sha256Actual != data.sha256Expected) {
            throw std::runtime_error("sha256 mismatch: while fetching " + url + ": expected " + std::string(data.sha256Expected) + " but got " + sha256Actual);
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
}



// generic parser for .gitattributes files
// I could not get libgit2 parsing to work with GitSourceAccessor,
// but GitExportIgnoreSourceAccessor somehow works, so TODO
AttrRule parseGitAttrLine(std::string_view line)
{
  // example .gitattributes line: `*.beeg filter=lfs diff=lfs merge=lfs -text`
    AttrRule rule;
    if (line.empty() || line[0] == '#')
        return rule;
    size_t pos = line.find_first_of(" \t");
    if (pos == std::string_view::npos)
        return rule;
    rule.pattern = std::string(line.substr(0, pos));
    pos = line.find_first_not_of(" \t", pos);
    if (pos == std::string_view::npos)
        return rule;
    std::string_view rest = line.substr(pos);

    while (!rest.empty()) {
        pos = rest.find_first_of(" \t");
        std::string_view attr = rest.substr(0, pos);
        if (attr[0] == '-') {
            rule.attributes[std::string(attr.substr(1))] = "false";
        } else if (auto equals_pos = attr.find('='); equals_pos != std::string_view::npos) {
            auto key = attr.substr(0, equals_pos);
            auto value = attr.substr(equals_pos + 1);
            rule.attributes[std::string(key)] = std::string(value);
        } else {
            rule.attributes[std::string(attr)] = "true";
        }
        if (pos == std::string_view::npos)
            break;
        rest = rest.substr(pos);
        pos = rest.find_first_not_of(" \t");
        if (pos == std::string_view::npos)
            break;
        rest = rest.substr(pos);
    }

    return rule;
}


std::vector<AttrRule> parseGitAttrFile(std::string_view content)
{
    std::vector<AttrRule> rules;

    size_t pos = 0;
    while (pos < content.length()) {
        size_t eol = content.find('\n', pos);
        std::string_view line;

        if (eol == std::string_view::npos) {
            line = content.substr(pos);
            pos = content.length();
        } else {
            line = content.substr(pos, eol - pos);
            pos = eol + 1;
        }

        // Trim carriage return if present
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);

        // Skip empty lines and comments
        if (line.empty() || line[0] == '#')
            continue;

        auto rule = parseGitAttrLine(line);
        if (!rule.pattern.empty())
            rules.push_back(std::move(rule));
    }

    return rules;
}




std::string getLfsApiToken(const std::string &host,
                              const std::string &path) {
  auto [status, output] = runProgram(RunOptions {
      .program = "ssh",
      .args = {"git@" + host, "git-lfs-authenticate", path, "download"},
  });

  std::string res;
  if (!output.empty()) {
    nlohmann::json query_resp = nlohmann::json::parse(output);
    res = query_resp["header"]["Authorization"].get<std::string>();
  }

  return res;
}

std::string getLfsEndpointUrl(git_repository *repo) {
  int err;
  git_remote* remote = NULL;
  err = git_remote_lookup(&remote, repo, "origin");
  if (err < 0) {
      std::cerr << " failed git_remote_lookup with: " << err << std::endl;
      return "";
  }


  const char *url_c_str = git_remote_url(remote);
  if (!url_c_str) {
    std::cerr << "no remote url ";
    return "";
  }

  return std::string(url_c_str);
}

// splits url into (hostname, path)
std::tuple<std::string, std::string> splitUrl(const std::string& url_in) {
  CURLU *url = curl_url();

  if (curl_url_set(url, CURLUPART_URL, url_in.c_str(), 0) != CURLUE_OK) {
      std::cerr << "Failed to set URL\n";
      return {"", ""};
  }

  char *hostname;
  char *path;

  if (curl_url_get(url, CURLUPART_HOST, &hostname, 0) != CURLUE_OK) {
      std::cerr << "no hostname" << std::endl;
  }

  if (curl_url_get(url, CURLUPART_PATH, &path, 0) != CURLUE_OK) {
      std::cerr << "no path" << std::endl;
  }

  return std::make_tuple(std::string(hostname), std::string(path));
}

std::string git_attr_value_to_string(git_attr_value_t value) {
  switch (value) {
  case GIT_ATTR_VALUE_UNSPECIFIED:
    return "GIT_ATTR_VALUE_UNSPECIFIED";
  case GIT_ATTR_VALUE_TRUE:
    return "GIT_ATTR_VALUE_TRUE";
  case GIT_ATTR_VALUE_FALSE:
    return "GIT_ATTR_VALUE_FALSE";
  case GIT_ATTR_VALUE_STRING:
    return "GIT_ATTR_VALUE_STRING";
  default:
    return "Unknown value";
  }
}

std::unique_ptr<std::vector<std::string>> find_lfs_files(git_repository *repo) {
  git_index *index;
  int error;
  error = git_repository_index(&index, repo);
  if (error < 0) {
    const git_error *e = git_error_last();
    std::cerr << "Error reading index: " << e->message << std::endl;
    git_repository_free(repo);
    return nullptr;
  }

  std::unique_ptr<std::vector<std::string>> out =
      std::make_unique<std::vector<std::string>>();
  size_t entry_count = git_index_entrycount(index);
  for (size_t i = 0; i < entry_count; ++i) {
    const git_index_entry *entry = git_index_get_byindex(index, i);
    if (entry) {
      const char *value;
      if (git_attr_get(&value, repo, GIT_ATTR_CHECK_INDEX_ONLY, entry->path,
                       "filter") == 0) {
        auto value_type = git_attr_value(value);
        if (value_type == GIT_ATTR_VALUE_STRING && strcmp(value, "lfs") == 0) {
          out->push_back(entry->path);
        }
      }
    }
  }
  return out;
}

Md parseLfsMetadata(const std::string &content, const std::string &filename) {
  // example git-lfs poitner file:
  // version https://git-lfs.github.com/spec/v1
  // oid sha256:f5e02aa71e67f41d79023a128ca35bad86cf7b6656967bfe0884b3a3c4325eaf
  // size 10000000
  std::istringstream iss(content);
  std::string line;
  std::string oid;
  size_t size = 0;

  while (getline(iss, line)) {
    std::size_t pos = line.find("oid sha256:");
    if (pos != std::string::npos) {
      oid = line.substr(pos + 11); // skip "oid sha256:"
      continue;
    }

    pos = line.find("size ");
    if (pos != std::string::npos) {
      std::string sizeStr =
          line.substr(pos + 5); // skip "size "
      size = std::stol(sizeStr);
      continue;
    }
  }

  return Md{filename, oid, size};
}

void Fetch::init(git_repository* repo, std::string gitattributesContent) {
   this->rootUrl = lfs::getLfsEndpointUrl(repo);
   const auto [host, path] = lfs::splitUrl(rootUrl);
   this->token = lfs::getLfsApiToken(host, path);
   this->rules = lfs::parseGitAttrFile(gitattributesContent);
   this->ready = true;
}

bool Fetch::hasAttribute(const std::string& path, const std::string& attrName) const
{
    // Iterate rules in reverse order (last matching rule wins)
    for (auto it = rules.rbegin(); it != rules.rend(); ++it) {
        if (matchesPattern(path, it->pattern)) {
            auto attr = it->attributes.find(attrName);
            if (attr != it->attributes.end()) {
                // Found a matching rule with this attribute
                return attr->second != "false";
            }
        }
    }
    return false;
}

nlohmann::json mdToPayload(const std::vector<Md> &items) {
  nlohmann::json jArray = nlohmann::json::array();
  for (const auto &md : items) {
    jArray.push_back({{"oid", md.oid}, {"size", md.size}});
  }
  return jArray;
}

std::vector<nlohmann::json> Fetch::fetchUrls(const std::vector<Md> &metadatas) const {
  nlohmann::json oidList = mdToPayload(metadatas);
  nlohmann::json data = {
      {"operation", "download"},
  };
  data["objects"] = oidList;
  auto dataStr = data.dump();

  CURL *curl = curl_easy_init();
  std::string responseString;
  std::string headerString;
  auto lfsUrlBatch = rootUrl + "/info/lfs/objects/batch";
  curl_easy_setopt(curl, CURLOPT_URL, lfsUrlBatch.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, dataStr.c_str());

  struct curl_slist *headers = NULL;
  auto authHeader = "Authorization: " + token;
  headers = curl_slist_append(headers, authHeader.c_str());
  headers =
      curl_slist_append(headers, "Content-Type: application/vnd.git-lfs+json");
  headers = curl_slist_append(headers, "Accept: application/vnd.git-lfs+json");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseString);

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK)
    fprintf(stderr, "curl_easy_perform() failed: %s\n",
            curl_easy_strerror(res));

  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);

  std::vector<nlohmann::json> objects;
  // example resp here:
  // {"objects":[{"oid":"f5e02aa71e67f41d79023a128ca35bad86cf7b6656967bfe0884b3a3c4325eaf","size":10000000,"actions":{"download":{"href":"https://gitlab.com/b-camacho/test-lfs.git/gitlab-lfs/objects/f5e02aa71e67f41d79023a128ca35bad86cf7b6656967bfe0884b3a3c4325eaf","header":{"Authorization":"Basic
  // Yi1jYW1hY2hvOmV5SjBlWEFpT2lKS1YxUWlMQ0poYkdjaU9pSklVekkxTmlKOS5leUprWVhSaElqcDdJbUZqZEc5eUlqb2lZaTFqWVcxaFkyaHZJbjBzSW1wMGFTSTZJbUptTURZNFpXVTFMVEprWmpVdE5HWm1ZUzFpWWpRMExUSXpNVEV3WVRReU1qWmtaaUlzSW1saGRDSTZNVGN4TkRZeE16ZzBOU3dpYm1KbUlqb3hOekUwTmpFek9EUXdMQ0psZUhBaU9qRTNNVFEyTWpFd05EVjkuZk9yMDNkYjBWSTFXQzFZaTBKRmJUNnJTTHJPZlBwVW9lYllkT0NQZlJ4QQ=="}}},"authenticated":true}]}
  auto resp = nlohmann::json::parse(responseString);
  if (resp.contains("objects")) {
    objects.insert(objects.end(), resp["objects"].begin(),
                   resp["objects"].end());
  } else {
    throw std::runtime_error("Response does not contain 'objects'");
  }

  return objects;
}

void Fetch::fetch(const std::string& pointerFileContents, const std::string& pointerFilePath, Sink& sink) const {
  const auto md = parseLfsMetadata(pointerFileContents, pointerFilePath);
  std::vector<Md> vMds;
  vMds.push_back(md);
  const auto objUrls = fetchUrls(vMds);

  const auto obj = objUrls[0];
  std::string oid = obj["oid"];
  std::string ourl = obj["actions"]["download"]["href"];
  std::string authHeader =
      obj["actions"]["download"]["header"]["Authorization"];
  // oid is also the sha256
  downloadToSink(ourl, authHeader, sink, oid);
}



} // namespace lfs

} // namespace nix