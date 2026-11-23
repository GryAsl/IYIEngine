// Exposes the localization subsystem to Python scripts under engine.loc.
// Mirrors the binding architecture of IYIUIBindings and EngineBindings.

#include "IYI/LocalizationManager.h"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

namespace IYI {

void RegisterLocalizationBindings(py::module_& m)
{
    py::module_ loc = m.def_submodule("loc", "IYI localization subsystem");

    loc.def("set_language", [](const std::string& code) {
        LocalizationManager::Get().SetLanguage(code);
    }, py::arg("code"), "Set the active language code");

    loc.def("get_language", []() {
        return LocalizationManager::Get().GetLanguage();
    }, "Get the active language code");

    loc.def("languages", []() {
        py::list list;
        for (const auto& info : LocalizationManager::Get().Languages()) {
            py::dict d;
            d["code"] = info.Code;
            d["name"] = info.Name;
            d["native_name"] = info.NativeName;
            list.append(d);
        }
        return list;
    }, "List all available languages as a list of dicts");

    loc.def("language_info", [](const std::string& code) -> py::object {
        const LocaleInfo* info = LocalizationManager::Get().GetLanguageInfo(code);
        if (!info) {
            return py::none();
        }
        py::dict d;
        d["code"] = info->Code;
        d["name"] = info->Name;
        d["native_name"] = info->NativeName;
        return d;
    }, py::arg("code"), "Get metadata dict for a language code, or None if unknown");

    loc.def("text", [](const std::string& key, const py::kwargs& kwargs) {
        std::unordered_map<std::string, std::string> args;
        for (auto item : kwargs) {
            std::string k = py::str(item.first);
            std::string v = py::str(item.second);
            args[k] = v;
        }
        return LocalizationManager::Get().Text(key, args);
    }, py::arg("key"), "Get localized text for key with optional placeholder arguments");

    loc.def("has", [](const std::string& key) {
        return LocalizationManager::Get().Has(key);
    }, py::arg("key"), "Check if a localization key exists");

    loc.def("get_fallback_language", []() {
        return LocalizationManager::Get().GetFallbackLanguage();
    }, "Get the fallback language code");

    loc.def("set_fallback_language", [](const std::string& code) {
        LocalizationManager::Get().SetFallbackLanguage(code);
    }, py::arg("code"), "Set the fallback language code");

    loc.def("missing_keys", []() {
        py::set keys;
        for (const auto& k : LocalizationManager::Get().MissingKeys()) {
            keys.add(k);
        }
        return keys;
    }, "Get set of missing localization keys");

    loc.def("reload", []() {
        LocalizationManager::Get().Reload();
    }, "Reload all locale files and catalog");
}

} // namespace IYI
