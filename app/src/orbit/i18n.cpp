// Orbit Store TV app - The interface language.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/i18n.hpp"

#include "core/save_file.hpp"
#include "orbit/json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <vector>

namespace orbit::i18n
{

namespace
{

constexpr std::array<std::string_view, 26> kCodes = {
    "en", "ar", "cs", "da", "de", "el", "es", "fi", "fr", "hu", "id", "it", "ja",
    "ko", "nb", "nl", "pl", "pt-BR", "pt-PT", "ro", "ru", "sv", "tr", "uk", "zh-Hans", "zh-Hant"};

// Plural forms in CLDR's order of categories; an empty form falls back to other.
enum Category : std::uint8_t
{
    zero,
    one,
    two,
    few,
    many,
    other,
};
using Forms = std::array<std::string, 6>;

bool is(std::initializer_list<std::string_view> codes);

struct Catalogue
{
    std::map<std::string, std::string, std::less<>> strings;
    std::map<std::string, Forms, std::less<>> plurals;
};

std::string current = "en";
std::string system_code = "en";
std::string folder_path;
std::uint64_t revision_count = 0;
// A language chosen in App settings replaces the catalogue while screens may still
// hold its text, so a replaced catalogue is kept, never freed. English has none.
std::vector<std::unique_ptr<Catalogue>> kept;
const Catalogue *active = nullptr;
const Catalogue no_catalogue;

const Catalogue &catalogue()
{
    return active != nullptr ? *active : no_catalogue;
}

bool is(std::initializer_list<std::string_view> codes)
{
    for (const std::string_view code : codes)
    {
        if (current == code)
            return true;
    }
    return false;
}

// The integer rules of the shipped languages, as Intl.PluralRules has them.
// French and Brazilian Portuguese treat 0 as singular; they, Spanish,
// Italian and European Portuguese use many for whole millions ("1 000 000
// de jeux"); Russian, Ukrainian and Polish have few and many; Czech and
// Romanian have few; Arabic has all six; Japanese, Korean, Chinese and
// Indonesian have one form.
Category category(long n)
{
    const long abs = n < 0 ? -n : n;
    const long ten = abs % 10;
    const long hundred = abs % 100;
    const bool millions = abs != 0 && abs % 1000000 == 0;
    if (is({"ja", "ko", "zh-Hans", "zh-Hant", "id"}))
        return other;
    if (current == "ar")
        return abs == 0 ? zero
               : abs == 1 ? one
               : abs == 2 ? two
               : hundred >= 3 && hundred <= 10 ? few
               : hundred >= 11 ? many
                                : other;
    if (current == "fr" || current == "pt-BR")
        return abs <= 1 ? one : millions ? many : other;
    if (is({"es", "it", "pt-PT"}))
        return abs == 1 ? one : millions ? many : other;
    if (current == "cs")
        return abs == 1 ? one : abs >= 2 && abs <= 4 ? few : other;
    if (current == "ro")
        return abs == 1 ? one : abs == 0 || (hundred >= 2 && hundred <= 19) ? few : other;
    if (current == "ru" || current == "uk")
    {
        if (ten == 1 && hundred != 11)
            return one;
        if (ten >= 2 && ten <= 4 && (hundred < 12 || hundred > 14))
            return few;
        return many;
    }
    if (current == "pl")
    {
        if (abs == 1)
            return one;
        if (ten >= 2 && ten <= 4 && (hundred < 12 || hundred > 14))
            return few;
        return many;
    }
    return abs == 1 ? one : other;
}

// One pass, so a filled-in value (a title, a path) is never filled again.
std::string fill(std::string_view text, std::initializer_list<Var> vars,
                 const Var *extra = nullptr)
{
    std::string out;
    out.reserve(text.size() + 16);
    std::size_t i = 0;
    while (i < text.size())
    {
        if (text[i] == '{')
        {
            const std::size_t close = text.find('}', i + 1);
            if (close != std::string_view::npos)
            {
                const std::string_view name = text.substr(i + 1, close - i - 1);
                const Var *match = nullptr;
                for (const Var &var : vars)
                {
                    if (var.name == name)
                        match = &var;
                }
                if (extra != nullptr && extra->name == name)
                    match = extra;
                if (match != nullptr)
                {
                    out += match->value;
                    i = close + 1;
                    continue;
                }
            }
        }
        out += text[i++];
    }
    return out;
}

// The app's fonts have no no-break space (French puts one before ? and :),
// so it becomes an ordinary space here.
std::string drawable(std::string text)
{
    for (std::size_t at = text.find("\xC2\xA0"); at != std::string::npos;
         at = text.find("\xC2\xA0", at + 1))
        text.replace(at, 2, " ");
    return text;
}

// The thousands separator, or empty when the language does not group.
std::string_view group_separator()
{
    if (is({"en", "ar", "ja", "ko", "zh-Hans", "zh-Hant"}))
        return ",";
    if (is({"fr", "hu", "pl", "ru", "cs", "fi", "nb", "pt-PT", "sv", "uk"}))
        return " "; // a space: the app's fonts have no no-break space
    return ".";
}

// Languages that put a space between the number and the percent sign.
bool spaced_percent()
{
    return is({"de", "es", "fr", "ru", "cs", "da", "fi", "nb", "ro", "sv"});
}

} // namespace

bool supported(std::string_view code)
{
    for (const std::string_view known : kCodes)
    {
        if (known == code)
            return true;
    }
    return false;
}

std::string_view from_system(int system_language)
{
    switch (system_language)
    {
    case 0:
        return "ja";
    case 2:  // French
    case 22: // French (Canada)
        return "fr";
    case 3:  // Spanish
    case 20: // Spanish (Latin America)
        return "es";
    case 4:
        return "de";
    case 5:
        return "it";
    case 6:
        return "nl";
    case 7:
        return "pt-PT";
    case 8:
        return "ru";
    case 9:
        return "ko";
    case 10:
        return "zh-Hant";
    case 11:
        return "zh-Hans";
    case 12:
        return "fi";
    case 13:
        return "sv";
    case 14:
        return "da";
    case 15:
        return "nb";
    case 16:
        return "pl";
    case 17:
        return "pt-BR";
    case 19:
        return "tr";
    case 21:
        return "ar";
    case 23:
        return "cs";
    case 24:
        return "hu";
    case 25:
        return "el";
    case 26:
        return "ro";
    case 29:
        return "id";
    case 30:
        return "uk";
    default: // English, and Thai and Vietnamese until they have catalogues
        return "en";
    }
}

bool load(const std::string &dir, std::string_view code)
{
    active = nullptr;
    current = "en";
    folder_path = dir;
    ++revision_count;
    if (!supported(code) || code == "en")
        return code == "en";
    std::string data;
    json::Value root;
    if (!hui::save::read_file(dir + "/" + std::string(code) + ".json", &data) ||
        !json::parse(data, &root) || !root.is_object())
        return false;
    auto loaded = std::make_unique<Catalogue>();
    const std::vector<std::string> &keys = root.keys();
    for (std::size_t k = 0; k < keys.size(); ++k)
    {
        const json::Value &value = root[keys[k]];
        if (value.is_string())
        {
            loaded->strings.emplace(keys[k], drawable(value.as_string()));
            continue;
        }
        Forms forms;
        forms[zero] = drawable(value.text("zero"));
        forms[one] = drawable(value.text("one"));
        forms[two] = drawable(value.text("two"));
        forms[few] = drawable(value.text("few"));
        forms[many] = drawable(value.text("many"));
        forms[other] = drawable(value.text("other"));
        loaded->plurals.emplace(keys[k], std::move(forms));
    }
    kept.push_back(std::move(loaded));
    active = kept.back().get();
    current = std::string(code);
    return true;
}

std::span<const Language> languages()
{
    // Each under its own name, in the browser's order (src/i18n.ts).
    static constexpr Language kLanguages[] = {
        {"en", "English"},
        {"id", "Bahasa Indonesia"},
        {"cs", "Čeština"},
        {"da", "Dansk"},
        {"de", "Deutsch"},
        {"es", "Español"},
        {"fr", "Français"},
        {"it", "Italiano"},
        {"hu", "Magyar"},
        {"nl", "Nederlands"},
        {"nb", "Norsk bokmål"},
        {"pl", "Polski"},
        {"pt-BR", "Português (Brasil)"},
        {"pt-PT", "Português (Portugal)"},
        {"ro", "Română"},
        {"fi", "Suomi"},
        {"sv", "Svenska"},
        {"tr", "Türkçe"},
        {"el", "Ελληνικά"},
        {"ru", "Русский"},
        {"uk", "Українська"},
        {"ar", "العربية"},
        {"ja", "日本語"},
        {"ko", "한국어"},
        {"zh-Hans", "简体中文"},
        {"zh-Hant", "繁體中文"}};
    return kLanguages;
}

void set_system(std::string_view code)
{
    system_code = supported(code) ? std::string(code) : "en";
}

std::string_view system()
{
    return system_code;
}

bool choose(std::string_view code)
{
    const std::string wanted(code.empty() ? std::string_view(system_code) : code);
    return wanted != current ? load(folder_path, wanted) : true;
}

std::uint64_t revision()
{
    return revision_count;
}

std::string_view language()
{
    return current;
}

const char *tr(const char *english)
{
    const auto &strings = catalogue().strings;
    const auto found = strings.find(std::string_view(english));
    return found == strings.end() ? english : found->second.c_str();
}

std::string_view tr(std::string_view english)
{
    const auto &strings = catalogue().strings;
    const auto found = strings.find(english);
    return found == strings.end() ? english : std::string_view(found->second);
}

std::string tr(std::string_view english, std::initializer_list<Var> vars)
{
    return fill(tr(english), vars);
}

std::string trn(long n, std::string_view one_form, std::string_view other_form,
                std::initializer_list<Var> vars)
{
    std::string_view text = n == 1 ? one_form : other_form;
    const auto &plurals = catalogue().plurals;
    const auto found = plurals.find(other_form);
    if (found != plurals.end())
    {
        const Forms &forms = found->second;
        const std::string &form = forms[category(n)];
        text = form.empty() ? std::string_view(forms[other]) : std::string_view(form);
    }
    const Var number{"count", count(n)};
    return fill(text, vars, &number);
}

std::string count(long value)
{
    const bool negative = value < 0;
    std::string digits = std::to_string(negative ? -value : value);
    // Spanish, Hungarian, Italian, Polish and European Portuguese group only
    // from five digits.
    const std::size_t from = is({"es", "hu", "it", "pl", "pt-PT"}) ? 5 : 4;
    if (digits.size() >= from)
    {
        const std::string_view separator = group_separator();
        std::string grouped;
        const std::size_t lead = digits.size() % 3 == 0 ? 3 : digits.size() % 3;
        grouped.append(digits, 0, lead);
        for (std::size_t i = lead; i < digits.size(); i += 3)
        {
            grouped += separator;
            grouped.append(digits, i, 3);
        }
        digits = std::move(grouped);
    }
    return negative ? "-" + digits : digits;
}

std::string decimal(double value)
{
    // Round first, then group the whole part like a count: 9.96 is 10.0.
    char text[48];
    std::snprintf(text, sizeof(text), "%.1f", std::fabs(value));
    const std::string_view rounded = text;
    const std::size_t point = rounded.find('.');
    const long whole = std::strtol(text, nullptr, 10);
    const std::string_view fraction =
        point == std::string_view::npos ? "0" : rounded.substr(point + 1);
    const bool full_stop = is({"en", "ar", "ja", "ko", "zh-Hans", "zh-Hant"});
    return (value < 0 && std::fabs(value) >= 0.05 ? "-" : "") + count(whole) +
           (full_stop ? "." : ",") + std::string(fraction);
}

std::string percent(double value)
{
    const std::string number = decimal(value);
    if (current == "tr")
        return "%" + number;
    return number + (spaced_percent() ? " %" : "%");
}

std::string whole_percent(double value)
{
    const std::string number = count(static_cast<long>(std::floor(std::max(0.0, value))));
    if (current == "tr")
        return "%" + number;
    return number + (spaced_percent() ? " %" : "%");
}

std::string place(std::string_view name)
{
    // "USB storage 2" from Orbit, "USB 2" and "Extended storage 1" from ShadowMount.
    const auto numbered = [&](std::string_view prefix, std::string *number) {
        if (name.size() <= prefix.size() || name.substr(0, prefix.size()) != prefix)
            return false;
        const std::string_view rest = name.substr(prefix.size());
        for (const char c : rest)
        {
            if (c < '0' || c > '9')
                return false;
        }
        *number = std::string(rest);
        return true;
    };
    std::string number;
    if (numbered("USB storage ", &number) || numbered("USB ", &number))
        return tr("USB storage {number}", {{"number", number}});
    if (numbered("Extended storage ", &number))
        return tr("Extended storage {number}", {{"number", number}});
    return std::string(tr(name));
}

std::string genre(std::string_view names)
{
    // Separated as game_genres() separates them: " / " or a comma.
    std::string out;
    for (std::size_t start = 0; start <= names.size();)
    {
        const std::size_t slash = names.find(" / ", start), comma = names.find(',', start);
        std::size_t end = std::min(slash, comma);
        const std::size_t skip = end == comma && end != std::string_view::npos ? 1 : 3;
        if (end == std::string_view::npos)
            end = names.size();
        std::string_view name = names.substr(start, end - start);
        const std::size_t first = name.find_first_not_of(" \t\r\n");
        if (first != std::string_view::npos)
        {
            name = name.substr(first, name.find_last_not_of(" \t\r\n") - first + 1);
            if (!out.empty()) out += " / ";
            out += tr(name);
        }
        start = end + skip;
    }
    return out;
}

std::string_view unit(bool giga)
{
    if (current == "ru" || current == "uk")
        return giga ? "\xD0\x93\xD0\x91" : "\xD0\x9C\xD0\x91"; // ГБ, МБ
    if (current == "fr")
        return giga ? "Go" : "Mo";
    if (current == "fi")
        return giga ? "Gt" : "Mt";
    if (current == "ar")
        return giga ? "غ.ب" : "م.ب";
    return giga ? "GB" : "MB";
}

std::string_view fallback_font(std::string_view code)
{
    if (code == "ja")
        return "noto-sans-jp.huifontz";
    if (code == "zh-Hans")
        return "noto-sans-sc.huifontz";
    if (code == "zh-Hant")
        return "noto-sans-tc.huifontz";
    if (code == "ko")
        return "noto-sans-kr.huifontz";
    if (code == "ar")
        return "noto-sans-arabic.huifontz";
    return {};
}

bool right_to_left()
{
    return current == "ar";
}

std::string_view unit_space()
{
    return current == "ko" ? "" : " "; // "49.4GB" in Korean
}

std::string date(int year, int month, int day)
{
    // Intl.DateTimeFormat(code, {dateStyle: "medium"}) month names.
    static const char *const kMonths[][12] = {
        {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"},
        {"ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sept", "oct", "nov", "dic"},
        {"janv.", "f\xC3\xA9vr.", "mars", "avr.", "mai", "juin", "juil.", "ao\xC3\xBBt", "sept.",
         "oct.", "nov.", "d\xC3\xA9" "c."},
        {"gen", "feb", "mar", "apr", "mag", "giu", "lug", "ago", "set", "ott", "nov", "dic"},
        {"jan", "feb", "mrt", "apr", "mei", "jun", "jul", "aug", "sep", "okt", "nov", "dec"},
        {"sty", "lut", "mar", "kwi", "maj", "cze", "lip", "sie", "wrz", "pa\xC5\xBA", "lis", "gru"},
        {"jan.", "fev.", "mar.", "abr.", "mai.", "jun.", "jul.", "ago.", "set.", "out.", "nov.",
         "dez."},
        {"\xD1\x8F\xD0\xBD\xD0\xB2.", "\xD1\x84\xD0\xB5\xD0\xB2\xD1\x80.",
         "\xD0\xBC\xD0\xB0\xD1\x80.", "\xD0\xB0\xD0\xBF\xD1\x80.", "\xD0\xBC\xD0\xB0\xD1\x8F",
         "\xD0\xB8\xD1\x8E\xD0\xBD.", "\xD0\xB8\xD1\x8E\xD0\xBB.", "\xD0\xB0\xD0\xB2\xD0\xB3.",
         "\xD1\x81\xD0\xB5\xD0\xBD\xD1\x82.", "\xD0\xBE\xD0\xBA\xD1\x82.",
         "\xD0\xBD\xD0\xBE\xD1\x8F\xD0\xB1.", "\xD0\xB4\xD0\xB5\xD0\xBA."},
        {"Oca", "\xC5\x9E" "ub", "Mar", "Nis", "May", "Haz", "Tem", "A\xC4\x9Fu", "Eyl", "Eki", "Kas",
         "Ara"},
        {"jan.", "febr.", "márc.", "ápr.", "máj.", "jún.", "júl.", "aug.", "szept.", "okt.", "nov.", "dec."},
    };
    // The new languages, also as Intl.DateTimeFormat's medium style has them.
    static const char *const kMore[][12] = {
        {"jan.", "feb.", "mar.", "apr.", "maj", "jun.", "jul.", "aug.", "sep.", "okt.", "nov.", "dec."},
        {"Ιαν", "Φεβ", "Μαρ", "Απρ", "Μαΐ", "Ιουν", "Ιουλ", "Αυγ", "Σεπ", "Οκτ", "Νοε", "Δεκ"},
        {"Jan", "Feb", "Mar", "Apr", "Mei", "Jun", "Jul", "Agu", "Sep", "Okt", "Nov", "Des"},
        {"jan.", "feb.", "mars", "apr.", "mai", "juni", "juli", "aug.", "sep.", "okt.", "nov.", "des."},
        {"ian.", "feb.", "mar.", "apr.", "mai", "iun.", "iul.", "aug.", "sept.", "oct.", "nov.", "dec."},
        {"jan.", "feb.", "mars", "apr.", "maj", "juni", "juli", "aug.", "sep.", "okt.", "nov.", "dec."},
        {"січ.", "лют.", "бер.", "квіт.", "трав.", "черв.", "лип.", "серп.", "вер.", "жовт.", "лист.", "груд."},
    };
    if (month < 1 || month > 12)
        return {};
    char text[64];
    const int m = month - 1;
    if (current == "de")
    {
        std::snprintf(text, sizeof(text), "%02d.%02d.%04d", day, month, year);
        return text;
    }
    if (current == "ar" || current == "pt-PT")
        std::snprintf(text, sizeof(text), "%02d/%02d/%04d", day, month, year);
    else if (current == "cs")
        std::snprintf(text, sizeof(text), "%d. %d. %04d", day, month, year);
    else if (current == "fi")
        std::snprintf(text, sizeof(text), "%d.%d.%04d", day, month, year);
    else if (current == "ja")
        std::snprintf(text, sizeof(text), "%04d/%02d/%02d", year, month, day);
    else if (current == "ko")
        std::snprintf(text, sizeof(text), "%04d. %d. %d.", year, month, day);
    else if (current == "zh-Hans" || current == "zh-Hant")
        std::snprintf(text, sizeof(text), "%04d年%d月%d日", year, month, day);
    else if (current == "da" || current == "nb")
        std::snprintf(text, sizeof(text), "%d. %s %04d", day, kMore[current == "da" ? 0 : 3][m], year);
    else if (current == "uk")
        std::snprintf(text, sizeof(text), "%d %s %04d р.", day, kMore[6][m], year);
    else if (is({"el", "id", "ro", "sv"}))
        std::snprintf(text, sizeof(text), "%d %s %04d", day,
                      kMore[current == "el" ? 1 : current == "id" ? 2 : current == "ro" ? 4 : 5][m], year);
    else
        text[0] = 0;
    if (text[0] != 0)
        return text;
    const int row = current == "es"      ? 1
                    : current == "fr"    ? 2
                    : current == "it"    ? 3
                    : current == "nl"    ? 4
                    : current == "pl"    ? 5
                    : current == "pt-BR" ? 6
                    : current == "ru"    ? 7
                    : current == "tr"    ? 8
                    : current == "hu"    ? 9
                                         : 0;
    if (row == 0)
        std::snprintf(text, sizeof(text), "%s %d, %04d", kMonths[0][m], day, year);
    else if (current == "pt-BR")
        std::snprintf(text, sizeof(text), "%d de %s de %04d", day, kMonths[row][m], year);
    else if (current == "ru")
        std::snprintf(text, sizeof(text), "%d %s %04d \xD0\xB3.", day, kMonths[row][m], year);
    else if (current == "hu")
        std::snprintf(text, sizeof(text), "%04d. %s %d.", year, kMonths[row][m], day);
    else
        std::snprintf(text, sizeof(text), "%d %s %04d", day, kMonths[row][m], year);
    return text;
}

} // namespace orbit::i18n
