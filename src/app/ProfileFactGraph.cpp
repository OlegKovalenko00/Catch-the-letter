#include "ProfileFactGraph.h"

#include <sstream>
#include <string>
#include <vector>

namespace {

std::string get_fact(const user_profile& profile, const std::string& key) {
  auto it = profile.values.find(key);
  if (it != profile.values.end() && !it->second.empty()) return it->second;
  auto it2 = profile.values.find("custom." + key);
  if (it2 != profile.values.end() && !it2->second.empty()) return it2->second;
  return "";
}

void set_if_missing(user_profile& profile, const std::string& key, const std::string& value) {
  if (value.empty()) return;
  if (!get_fact(profile, key).empty()) return;
  profile.values[key] = value;
}

std::vector<std::string> split_words(const std::string& s) {
  std::vector<std::string> parts;
  std::istringstream iss(s);
  std::string word;
  while (iss >> word) parts.push_back(word);
  return parts;
}


std::string first_letter_dot(const std::string& word) {
  if (word.empty()) return "";
  const unsigned char c = static_cast<unsigned char>(word[0]);
  std::string ch;
  if (c < 0x80) ch = word.substr(0, 1);
  else if (c < 0xE0 && word.size() >= 2) ch = word.substr(0, 2);
  else if (c < 0xF0 && word.size() >= 3) ch = word.substr(0, 3);
  else if (word.size() >= 4) ch = word.substr(0, 4);
  return ch + ".";
}

}

void expand_profile_facts(user_profile& profile) {

  const std::string full = get_fact(profile, "full_name");
  if (!full.empty()) {
    const auto parts = split_words(full);
    if (parts.size() >= 1) set_if_missing(profile, "last_name",  parts[0]);
    if (parts.size() >= 2) set_if_missing(profile, "first_name", parts[1]);
    if (parts.size() >= 3) set_if_missing(profile, "middle_name", parts[2]);
    if (parts.size() >= 2) {

      std::string initials = parts[0];
      for (size_t i = 1; i < parts.size(); ++i)
        initials += " " + first_letter_dot(parts[i]);
      set_if_missing(profile, "initials", initials);
    }
  }


  if (full.empty()) {
    const std::string last   = get_fact(profile, "last_name");
    const std::string first  = get_fact(profile, "first_name");
    const std::string middle = get_fact(profile, "middle_name");
    if (!last.empty() && !first.empty()) {
      const std::string composed = last + " " + first + (middle.empty() ? "" : " " + middle);
      set_if_missing(profile, "full_name", composed);
      set_if_missing(profile, "first_last", first + " " + last);
      set_if_missing(profile, "last_first", last + " " + first);
    }
  }


  const std::string sex = get_fact(profile, "sex");
  if (sex == "male" || sex == "м" || sex == "мужской" || sex == "мужчина" ||
      sex == "man" || sex == "муж." || sex == "м.") {
    set_if_missing(profile, "sex_ru",        "Мужской");
    set_if_missing(profile, "sex_short_ru",  "М");
    set_if_missing(profile, "sex_en",        "Male");
    set_if_missing(profile, "sex_canonical", "male");
  } else if (sex == "female" || sex == "ж" || sex == "женский" || sex == "женщина" ||
             sex == "woman" || sex == "жен." || sex == "ж.") {
    set_if_missing(profile, "sex_ru",        "Женский");
    set_if_missing(profile, "sex_short_ru",  "Ж");
    set_if_missing(profile, "sex_en",        "Female");
    set_if_missing(profile, "sex_canonical", "female");
  }


  const std::string personal = get_fact(profile, "personal_email");
  if (!personal.empty()) {
    set_if_missing(profile, "email",         personal);
    set_if_missing(profile, "primary_email", personal);
  }
  const std::string hse = get_fact(profile, "hse_email");
  if (!hse.empty()) {
    set_if_missing(profile, "corporate_email", hse);
    set_if_missing(profile, "edu_email",       hse);
  }


  const std::string group = get_fact(profile, "student_group");
  if (!group.empty()) {
    set_if_missing(profile, "group",          group);
    set_if_missing(profile, "academic_group", group);
  }
}
