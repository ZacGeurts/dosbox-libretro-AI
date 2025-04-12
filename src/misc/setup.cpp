/*
 *  Copyright (C) 2002-2025  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "dosbox.h"
#include "cross.h"
#include "setup.h"
#include "control.h"
#include "support.h"
#include <fstream>
#include <string>
#include <sstream>
#include <list>
#include <cstdlib>
#include <limits>
#include <cctype>
#include <algorithm>

using namespace std;

static std::string current_config_dir;

void Value::destroy() noexcept {
    if (type == V_STRING) {
        delete _string;
        _string = nullptr;
    }
}

Value& Value::copy(const Value& in) noexcept {
    if (this != &in) {
        if (type != V_NONE && type != in.type) {
            return *this;
        }
        destroy();
        plaincopy(in);
    }
    return *this;
}

void Value::plaincopy(const Value& in) noexcept {
    type = in.type;
    _int = in._int;
    _double = in._double;
    _bool = in._bool;
    _hex = in._hex;
    if (type == V_STRING) {
        _string = new string(*in._string);
    }
}

Value::operator bool() const noexcept {
    return type == V_BOOL ? _bool : false;
}

Value::operator Hex() const noexcept {
    return type == V_HEX ? _hex : static_cast<Hex>(0);
}

Value::operator int() const noexcept {
    return type == V_INT ? _int : 0;
}

Value::operator double() const noexcept {
    return type == V_DOUBLE ? _double : 0.0;
}

Value::operator const char*() const noexcept {
    return type == V_STRING ? _string->c_str() : "";
}

bool Value::operator==(const Value& other) const {
    if (this == &other) {
        return true;
    }
    if (type != other.type) {
        return false;
    }
    switch (type) {
        case V_BOOL:   return _bool == other._bool;
        case V_INT:    return _int == other._int;
        case V_HEX:    return _hex == other._hex;
        case V_DOUBLE: return _double == other._double;
        case V_STRING: return *_string == *other._string;
        default:       E_Exit("Comparing invalid Value types");
    }
    return false;
}

bool Value::SetValue(const string& in, Etype _type) noexcept {
    if (_type == V_CURRENT && type == V_NONE) {
        LOG_MSG("CONFIG: Invalid type for setting value");
        return false;
    }
    if (_type != V_CURRENT) {
        if (type != V_NONE && type != _type) {
            LOG_MSG("CONFIG: Type mismatch for value %s", in.c_str());
            return false;
        }
        destroy();
        type = _type;
    }
    switch (type) {
        case V_HEX:
            return set_hex(in);
        case V_INT:
            return set_int(in);
        case V_BOOL:
            return set_bool(in);
        case V_STRING:
            set_string(in);
            return true;
        case V_DOUBLE:
            return set_double(in);
        default:
            LOG_MSG("CONFIG: Unsupported type %d", static_cast<int>(type));
            return false;
    }
}

bool Value::set_hex(const string& in) {
    istringstream input(in);
    input >> hex;
    Bits result;
    input >> result;
    if (input.fail() || !input.eof()) {
        return false;
    }
    _hex = result;
    return true;
}

bool Value::set_int(const string& in) {
    istringstream input(in);
    Bits result;
    input >> result;
    if (input.fail() || !input.eof()) {
        return false;
    }
    _int = result;
    return true;
}

bool Value::set_double(const string& in) {
    char* endptr;
    errno = 0;
    double result = strtod(in.c_str(), &endptr);
    if (*endptr != '\0' || in.empty() || errno == ERANGE) {
        return false;
    }
    _double = result;
    return true;
}

bool Value::set_bool(const string& in) {
    string temp = in;
    lowcase(temp);
    if (temp.empty()) {
        return false;
    }
    if (temp == "0" || temp == "disabled" || temp == "false" || temp == "off") {
        _bool = false;
        return true;
    }
    if (temp == "1" || temp == "enabled" || temp == "true" || temp == "on") {
        _bool = true;
        return true;
    }
    return false;
}

void Value::set_string(const string& in) {
    if (!_string) {
        _string = new string();
    }
    *_string = in;
}

string Value::ToString() const {
    ostringstream oss;
    switch (type) {
        case V_HEX:
            oss << hex << _hex;
            break;
        case V_INT:
            oss << _int;
            break;
        case V_BOOL:
            oss << boolalpha << _bool;
            break;
        case V_STRING:
            oss << *_string;
            break;
        case V_DOUBLE:
            oss.precision(2);
            oss << fixed << _double;
            break;
        default:
            E_Exit("Invalid Value type for ToString");
    }
    return oss.str();
}

bool Property::CheckValue(const Value& in, bool warn) {
    if (suggested_values.empty()) {
        return true;
    }
    for (const auto& val : suggested_values) {
        if (val == in) {
            return true;
        }
    }
    if (warn) {
        LOG_MSG("CONFIG: Value \"%s\" invalid for %s; resetting to default: %s",
                in.ToString().c_str(), propname.c_str(), default_value.ToString().c_str());
    }
    return false;
}

void Property::Set_help(const string& in) {
    string key = "CONFIG_" + propname;
    upcase(key);
    MSG_Add(key.c_str(), in.c_str());
}

char const* Property::Get_help() {
    string key = "CONFIG_" + propname;
    upcase(key);
    return MSG_Get(key.c_str());
}

bool Prop_int::SetVal(const Value& in, bool forced, bool warn) {
    if (forced) {
        value = in;
        return true;
    }
    if (!suggested_values.empty()) {
        if (CheckValue(in, warn)) {
            value = in;
            return true;
        }
        value = default_value;
        return false;
    }
    int val = static_cast<int>(in);
    int min_val = static_cast<int>(min);
    int max_val = static_cast<int>(max);
    if (min_val == -1 && max_val == -1) {
        value = in;
        return true;
    }
    if (val >= min_val && val <= max_val) {
        value = in;
        return true;
    }
    int adjusted = (val > max_val) ? max_val : min_val;
    if (warn) {
        LOG_MSG("CONFIG: Value %s outside range %d-%d for %s; set to %d",
                in.ToString().c_str(), min_val, max_val, propname.c_str(), adjusted);
    }
    value = adjusted;
    return true;
}

bool Prop_int::CheckValue(const Value& in, bool warn) {
    if (!suggested_values.empty()) {
        return Property::CheckValue(in, warn);
    }
    int val = static_cast<int>(in);
    int min_val = static_cast<int>(min);
    int max_val = static_cast<int>(max);
    if (min_val == -1 && max_val == -1) {
        return true;
    }
    if (val >= min_val && val <= max_val) {
        return true;
    }
    if (warn) {
        LOG_MSG("CONFIG: Value %s outside range %d-%d for %s; default is %s",
                in.ToString().c_str(), min_val, max_val, propname.c_str(), default_value.ToString().c_str());
    }
    return false;
}

bool Prop_int::SetValue(const string& input) {
    Value val;
    if (!val.SetValue(input, Value::V_INT)) {
        return false;
    }
    return SetVal(val, false, true);
}

bool Prop_double::SetValue(const string& input) {
    Value val;
    if (!val.SetValue(input, Value::V_DOUBLE)) {
        return false;
    }
    return SetVal(val, false, true);
}

bool Prop_string::SetValue(const string& input) {
    string temp = input;
    if (!suggested_values.empty()) {
        lowcase(temp);
    }
    Value val(temp, Value::V_STRING);
    return SetVal(val, false, true);
}

bool Prop_string::CheckValue(const Value& in, bool warn) {
    if (suggested_values.empty()) {
        return true;
    }
    for (const auto& val : suggested_values) {
        if (val == in) {
            return true;
        }
        if (val.ToString() == "%u") {
            unsigned int dummy;
            if (sscanf(in.ToString().c_str(), "%u", &dummy) == 1) {
                return true;
            }
        }
    }
    if (warn) {
        LOG_MSG("CONFIG: Value \"%s\" invalid for %s; resetting to default: %s",
                in.ToString().c_str(), propname.c_str(), default_value.ToString().c_str());
    }
    return false;
}

bool Prop_path::SetValue(const string& input) {
    Value val(input, Value::V_STRING);
    bool retval = SetVal(val, false, true);
    if (input.empty()) {
        realpath.clear();
        return false;
    }
    string workcopy = input;
    Cross::ResolveHomedir(workcopy);
    realpath = Cross::IsPathAbsolute(workcopy) ? workcopy :
               current_config_dir.empty() ? workcopy :
               current_config_dir + CROSS_FILESPLIT + workcopy;
    return retval;
}

bool Prop_bool::SetValue(const string& input) {
    Value val;
    if (!val.SetValue(input, Value::V_BOOL)) {
        return false;
    }
    return SetVal(val, false, true);
}

bool Prop_hex::SetValue(const string& input) {
    Value val;
    if (!val.SetValue(input, Value::V_HEX)) {
        return false;
    }
    return SetVal(val, false, true);
}

void Prop_multival::make_default_value() {
    string result;
    for (size_t i = 0; ; ++i) {
        Property* p = section->Get_prop(i);
        if (!p) {
            break;
        }
        string props = p->Get_Default_Value().ToString();
        if (props.empty()) {
            continue;
        }
        if (!result.empty()) {
            result += separator;
        }
        result += props;
    }
    Value val(result, Value::V_STRING);
    SetVal(val, false, true);
}

bool Prop_multival::SetValue(const string& input) {
    Value val(input, Value::V_STRING);
    bool retval = SetVal(val, false, true);
    string local = input;
    size_t i = 0;
    while (Property* p = section->Get_prop(i++)) {
        size_t loc = local.find_first_not_of(separator);
        if (loc != string::npos) {
            local.erase(0, loc);
        }
        loc = local.find_first_of(separator);
        string in = loc != string::npos ? local.substr(0, loc) : local;
        if (loc != string::npos) {
            local.erase(0, loc + 1);
        } else {
            local.clear();
        }
        Value valtest(in, p->Get_type());
        if (!p->CheckValue(valtest, true)) {
            make_default_value();
            return false;
        }
        p->SetValue(in);
    }
    return retval;
}

bool Prop_multival_remain::SetValue(const string& input) {
    Value val(input, Value::V_STRING);
    bool retval = SetVal(val, false, true);
    string local = input;
    size_t num_props = 0;
    while (section->Get_prop(num_props)) {
        ++num_props;
    }
    size_t i = 0;
    while (Property* p = section->Get_prop(i++)) {
        size_t loc = local.find_first_not_of(separator);
        if (loc != string::npos) {
            local.erase(0, loc);
        }
        loc = local.find_first_of(separator);
        string in = (loc != string::npos && i < num_props) ? local.substr(0, loc) : local;
        if (loc != string::npos) {
            local.erase(0, loc + 1);
        } else {
            local.clear();
        }
        Value valtest(in, p->Get_type());
        if (!p->CheckValue(valtest, true)) {
            make_default_value();
            return false;
        }
        p->SetValue(in);
    }
    return retval;
}

const vector<Value>& Property::GetValues() const {
    return suggested_values;
}

const vector<Value>& Prop_multival::GetValues() const {
    for (size_t i = 0; ; ++i) {
        Property* p = section->Get_prop(i);
        if (!p) {
            break;
        }
        const auto& vals = p->GetValues();
        if (!vals.empty()) {
            return vals;
        }
    }
    return suggested_values;
}

void Property::Set_values(const char* const* in) {
    Value::Etype type = default_value.type;
    for (size_t i = 0; in[i]; ++i) {
        suggested_values.emplace_back(in[i], type);
    }
}

Prop_int* Section_prop::Add_int(const string& _propname, Property::Changeable::Value when, int _value) {
    auto* test = new Prop_int(_propname, when, _value);
    properties.push_back(test);
    return test;
}

Prop_string* Section_prop::Add_string(const string& _propname, Property::Changeable::Value when, const char* const _value) {
    auto* test = new Prop_string(_propname, when, _value);
    properties.push_back(test);
    return test;
}

Prop_path* Section_prop::Add_path(const string& _propname, Property::Changeable::Value when, const char* const _value) {
    auto* test = new Prop_path(_propname, when, _value);
    properties.push_back(test);
    return test;
}

Prop_bool* Section_prop::Add_bool(const string& _propname, Property::Changeable::Value when, bool _value) {
    auto* test = new Prop_bool(_propname, when, _value);
    properties.push_back(test);
    return test;
}

Prop_hex* Section_prop::Add_hex(const string& _propname, Property::Changeable::Value when, Hex _value) {
    auto* test = new Prop_hex(_propname, when, _value);
    properties.push_back(test);
    return test;
}

Prop_multival* Section_prop::Add_multi(const string& _propname, Property::Changeable::Value when, const string& sep) {
    auto* test = new Prop_multival(_propname, when, sep);
    properties.push_back(test);
    return test;
}

Prop_multival_remain* Section_prop::Add_multiremain(const string& _propname, Property::Changeable::Value when, const string& sep) {
    auto* test = new Prop_multival_remain(_propname, when, sep);
    properties.push_back(test);
    return test;
}

int Section_prop::Get_int(const string& _propname) const {
    for (const auto* prop : properties) {
        if (prop->propname == _propname) {
            return static_cast<int>(prop->GetValue());
        }
    }
    return 0;
}

bool Section_prop::Get_bool(const string& _propname) const {
    for (const auto* prop : properties) {
        if (prop->propname == _propname) {
            return static_cast<bool>(prop->GetValue());
        }
    }
    return false;
}

double Section_prop::Get_double(const string& _propname) const {
    for (const auto* prop : properties) {
        if (prop->propname == _propname) {
            return static_cast<double>(prop->GetValue());
        }
    }
    return 0.0;
}

Prop_path* Section_prop::Get_path(const string& _propname) const {
    for (const auto* prop : properties) {
        if (prop->propname == _propname) {
            return const_cast<Prop_path*>(dynamic_cast<const Prop_path*>(prop));
        }
    }
    return nullptr;
}

Prop_multival* Section_prop::Get_multival(const string& _propname) const {
    for (const auto* prop : properties) {
        if (prop->propname == _propname) {
            return const_cast<Prop_multival*>(dynamic_cast<const Prop_multival*>(prop));
        }
    }
    return nullptr;
}

Prop_multival_remain* Section_prop::Get_multivalremain(const string& _propname) const {
    for (const auto* prop : properties) {
        if (prop->propname == _propname) {
            return const_cast<Prop_multival_remain*>(dynamic_cast<const Prop_multival_remain*>(prop));
        }
    }
    return nullptr;
}

Property* Section_prop::Get_prop(int index) {
    for (auto* prop : properties) {
        if (index-- == 0) {
            return prop;
        }
    }
    return nullptr;
}

const char* Section_prop::Get_string(const string& _propname) const {
    for (const auto* prop : properties) {
        if (prop->propname == _propname) {
            return static_cast<const char*>(prop->GetValue());
        }
    }
    return "";
}

Hex Section_prop::Get_hex(const string& _propname) const {
    for (const auto* prop : properties) {
        if (prop->propname == _propname) {
            return static_cast<Hex>(prop->GetValue());
        }
    }
    return 0;
}

bool Section_prop::HandleInputline(const string& line) {
    string name, val;
    auto loc = line.find('=');
    if (loc == string::npos) {
        return false;
    }
    name = line.substr(0, loc);
    val = line.substr(loc + 1);

    char name_buf[1024], val_buf[1024];
    strncpy(name_buf, name.c_str(), sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = '\0';
    strncpy(val_buf, val.c_str(), sizeof(val_buf) - 1);
    val_buf[sizeof(val_buf) - 1] = '\0';

    trim(name_buf);
    trim(val_buf);

    name = name_buf;
    val = val_buf;

    if (val.length() > 1 && ((val.front() == '"' && val.back() == '"') || (val.front() == '\'' && val.back() == '\''))) {
        val = val.substr(1, val.length() - 2);
    }
    for (auto* prop : properties) {
        if (strcasecmp(prop->propname.c_str(), name.c_str()) == 0) {
            return prop->SetValue(val);
        }
    }
    return false;
}

void Section_prop::PrintData(FILE* outfile) const {
    for (const auto* prop : properties) {
        fprintf(outfile, "%s=%s\n", prop->propname.c_str(), prop->GetValue().ToString().c_str());
    }
}

string Section_prop::GetPropValue(const string& _property) const {
    for (const auto* prop : properties) {
        if (strcasecmp(prop->propname.c_str(), _property.c_str()) == 0) {
            return prop->GetValue().ToString();
        }
    }
    return NO_SUCH_PROPERTY;
}

bool Section_line::HandleInputline(const string& line) {
    data += line + "\n";
    return true;
}

void Section_line::PrintData(FILE* outfile) const {
    fprintf(outfile, "%s", data.c_str());
}

string Section_line::GetPropValue(const string& /*_property*/) const {
    return NO_SUCH_PROPERTY;
}

bool Config::PrintConfig(const char* const configfilename) const {
    FILE* outfile = fopen(configfilename, "w");
    if (!outfile) {
        return false;
    }
    fprintf(outfile, MSG_Get("CONFIGFILE_INTRO"), VERSION);
    fprintf(outfile, "\n");
    for (const auto* sec : sectionlist) {
        string sec_name = sec->GetName();
        char sec_name_buf[1024];
        strncpy(sec_name_buf, sec_name.c_str(), sizeof(sec_name_buf) - 1);
        sec_name_buf[sizeof(sec_name_buf) - 1] = '\0';
        lowcase(sec_name_buf);
        sec_name = sec_name_buf;
        fprintf(outfile, "[%s]\n", sec_name.c_str());
        if (auto* prop_sec = dynamic_cast<Section_prop*>(const_cast<Section*>(sec))) {
            size_t maxwidth = 0;
            for (size_t i = 0; Property* p = prop_sec->Get_prop(i); ++i) {
                maxwidth = max(maxwidth, p->propname.length());
            }
            for (size_t i = 0; Property* p = prop_sec->Get_prop(i); ++i) {
                string help = p->Get_help();
                string prefix = "\n# " + string(maxwidth, ' ') + "  ";
                size_t pos = 0;
                while ((pos = help.find('\n', pos + 1)) != string::npos) {
                    help.replace(pos, 1, prefix);
                }
                fprintf(outfile, "# %*s: %s", static_cast<int>(maxwidth), p->propname.c_str(), help.c_str());
                const auto& values = p->GetValues();
                if (!values.empty()) {
                    fprintf(outfile, "%s%s:", prefix.c_str(), MSG_Get("CONFIG_SUGGESTED_VALUES"));
                    bool first = true;
                    for (const auto& val : values) {
                        if (val.ToString() != "%u") {
                            fprintf(outfile, "%s%s", first ? " " : ", ", val.ToString().c_str());
                            first = false;
                        }
                    }
                    fprintf(outfile, ".");
                }
                fprintf(outfile, "\n");
            }
        } else {
            string key = string(sec->GetName()) + "_CONFIGFILE_HELP";
            upcase(key);
            string help = MSG_Get(key.c_str());
            string prefix = "# ";
            size_t pos = 0;
            while ((pos = help.find('\n', pos + 1)) != string::npos) {
                help.replace(pos, 1, "\n" + prefix);
            }
            fprintf(outfile, "%s%s\n", prefix.c_str(), help.c_str());
        }
        sec->PrintData(outfile);
        fprintf(outfile, "\n");
    }
    fclose(outfile);
    return true;
}

Section_prop* Config::AddSection_prop(const char* const _name, void (*_initfunction)(Section*), bool canchange) {
    auto* blah = new Section_prop(_name);
    blah->AddInitFunction(_initfunction, canchange);
    sectionlist.push_back(blah);
    return blah;
}

Section_line* Config::AddSection_line(const char* const _name, void (*_initfunction)(Section*)) {
    auto* blah = new Section_line(_name);
    blah->AddInitFunction(_initfunction);
    sectionlist.push_back(blah);
    return blah;
}

Section_prop::~Section_prop() {
    ExecuteDestroy(true);
    for (auto* prop : properties) {
        delete prop;
    }
}

void Config::Init() {
    for (auto* sec : sectionlist) {
        sec->ExecuteInit();
    }
}

void Section::AddInitFunction(SectionFunction func, bool canchange) {
    initfunctions.emplace_back(func, canchange);
}

void Section::AddDestroyFunction(SectionFunction func, bool canchange) {
    destroyfunctions.emplace_front(func, canchange);
}

void Section::ExecuteInit(bool initall) {
    for (const auto& wrapper : initfunctions) {
        if (initall || wrapper.canchange) {
            wrapper.function(this);
        }
    }
}

void Section::ExecuteDestroy(bool destroyall) {
    for (auto it = destroyfunctions.begin(); it != destroyfunctions.end(); ) {
        if (destroyall || it->canchange) {
            it->function(this);
            it = destroyfunctions.erase(it);
        } else {
            ++it;
        }
    }
}

Config::~Config() {
    for (auto it = sectionlist.rbegin(); it != sectionlist.rend(); ++it) {
        delete *it;
    }
}

Section* Config::GetSection(const string& _sectionname) const {
    for (const auto* sec : sectionlist) {
        if (strcasecmp(sec->GetName(), _sectionname.c_str()) == 0) {
            return const_cast<Section*>(sec);
        }
    }
    return nullptr;
}

Section* Config::GetSectionFromProperty(const char* const prop) const {
    for (const auto* sec : sectionlist) {
        if (sec->GetPropValue(prop) != NO_SUCH_PROPERTY) {
            return const_cast<Section*>(sec);
        }
    }
    return nullptr;
}

bool Config::ParseConfigFile(const char* const configfilename) {
    ifstream in(configfilename);
    if (!in) {
        return false;
    }
    const char* settings_type = configfiles.empty() ? "primary" : "additional";
    configfiles.push_back(configfilename);
    LOG_MSG("CONFIG: Loading %s settings from %s", settings_type, configfilename);

    current_config_dir = configfilename;
    auto pos = current_config_dir.rfind(CROSS_FILESPLIT);
    current_config_dir.erase(pos == string::npos ? 0 : pos);

    string line;
    Section* currentsection = nullptr;
    while (getline(in, line)) {
        char line_buf[1024];
        strncpy(line_buf, line.c_str(), sizeof(line_buf) - 1);
        line_buf[sizeof(line_buf) - 1] = '\0';
        trim(line_buf);
        line = line_buf;

        if (line.empty() || line[0] == '%' || line[0] == '#' || line[0] == '\n') {
            continue;
        }
        if (line[0] == '[') {
            auto loc = line.find(']');
            if (loc == string::npos) {
                continue;
            }
            string section_name = line.substr(1, loc - 1);
            if (Section* sec = GetSection(section_name)) {
                currentsection = sec;
            }
            continue;
        }
        if (currentsection) {
            try {
                currentsection->HandleInputline(line);
            } catch (const char*) {
                // Ignore parsing errors
            }
        }
    }
    current_config_dir.clear();
    return true;
}

void Config::ParseEnv(char** envp) {
    for (; *envp; ++envp) {
        string env = *envp;
        if (env.length() <= 7 || env.compare(0, 7, "DOSBOX_") != 0) {
            continue;
        }
        env.erase(0, 7);
        auto pos = env.rfind('_');
        if (pos == string::npos) {
            continue;
        }
        string sec_name = env.substr(0, pos);
        string prop_name = env.substr(pos + 1);
        if (Section* sec = GetSection(sec_name)) {
            sec->HandleInputline(prop_name);
        }
    }
}

void Config::SetStartUp(void (*_function)()) {
    _start_function = _function;
}

void Config::StartUp() {
    initialised = true;
    (*_start_function)();
}

bool CommandLine::FindExist(const char* const name, bool remove) {
    auto it = std::find_if(cmds.begin(), cmds.end(),
                           [name](const string& cmd) { return strcasecmp(cmd.c_str(), name) == 0; });
    if (it == cmds.end()) {
        return false;
    }
    if (remove) {
        cmds.erase(it);
    }
    return true;
}

bool CommandLine::FindHex(const char* const name, int& value, bool remove) {
    auto it = std::find_if(cmds.begin(), cmds.end(),
                           [name](const string& cmd) { return strcasecmp(cmd.c_str(), name) == 0; });
    if (it == cmds.end() || std::next(it) == cmds.end()) {
        return false;
    }
    auto it_next = std::next(it);
    if (sscanf(it_next->c_str(), "%X", &value) != 1) {
        return false;
    }
    if (remove) {
        cmds.erase(it, std::next(it_next));
    }
    return true;
}

bool CommandLine::FindInt(const char* const name, int& value, bool remove) {
    auto it = std::find_if(cmds.begin(), cmds.end(),
                           [name](const string& cmd) { return strcasecmp(cmd.c_str(), name) == 0; });
    if (it == cmds.end() || std::next(it) == cmds.end()) {
        return false;
    }
    auto it_next = std::next(it);
    value = atoi(it_next->c_str());
    if (remove) {
        cmds.erase(it, std::next(it_next));
    }
    return true;
}

bool CommandLine::FindString(const char* const name, string& value, bool remove) {
    auto it = std::find_if(cmds.begin(), cmds.end(),
                           [name](const string& cmd) { return strcasecmp(cmd.c_str(), name) == 0; });
    if (it == cmds.end() || std::next(it) == cmds.end()) {
        return false;
    }
    auto it_next = std::next(it);
    value = *it_next;
    if (remove) {
        cmds.erase(it, std::next(it_next));
    }
    return true;
}

bool CommandLine::FindCommand(unsigned int which, string& value) {
    if (which < 1 || which > cmds.size()) {
        return false;
    }
    auto it = cmds.begin();
    std::advance(it, which - 1);
    value = *it;
    return true;
}

bool CommandLine::FindEntry(const char* const name, cmd_it& it, bool neednext) {
    it = std::find_if(cmds.begin(), cmds.end(),
                      [name](const string& cmd) { return strcasecmp(cmd.c_str(), name) == 0; });
    if (it == cmds.end()) {
        return false;
    }
    if (neednext && std::next(it) == cmds.end()) {
        return false;
    }
    return true;
}

bool CommandLine::FindStringBegin(const char* const begin, string& value, bool remove) {
    size_t len = strlen(begin);
    auto it = std::find_if(cmds.begin(), cmds.end(),
                           [begin, len](const string& cmd) { return strncmp(begin, cmd.c_str(), len) == 0; });
    if (it == cmds.end()) {
        return false;
    }
    value = it->substr(len);
    if (remove) {
        cmds.erase(it);
    }
    return true;
}

bool CommandLine::FindStringRemain(const char* const name, string& value) {
    auto it = std::find_if(cmds.begin(), cmds.end(),
                           [name](const string& cmd) { return strcasecmp(cmd.c_str(), name) == 0; });
    if (it == cmds.end()) {
        return false;
    }
    value.clear();
    for (auto it_next = std::next(it); it_next != cmds.end(); ++it_next) {
        if (!value.empty()) {
            value += " ";
        }
        value += *it_next;
    }
    return true;
}

bool CommandLine::FindStringRemainBegin(const char* const name, string& value) {
    size_t len = strlen(name);
    auto it = std::find_if(cmds.begin(), cmds.end(),
                           [name, len](const string& cmd) { return strncasecmp(name, cmd.c_str(), len) == 0; });
    if (it == cmds.end()) {
        return false;
    }
    string temp = it->substr(len);
    value = temp.find(' ') != string::npos ? "\"" + temp + "\"" : temp;
    for (auto it_next = std::next(it); it_next != cmds.end(); ++it_next) {
        value += " ";
        temp = *it_next;
        value += temp.find(' ') != string::npos ? "\"" + temp + "\"" : temp;
    }
    return true;
}

bool CommandLine::GetStringRemain(string& value) {
    if (cmds.empty()) {
        return false;
    }
    auto it = cmds.begin();
    value = *it;
    for (++it; it != cmds.end(); ++it) {
        value += " " + *it;
    }
    return true;
}

unsigned int CommandLine::GetCount() {
    return static_cast<unsigned int>(cmds.size());
}

void CommandLine::FillVector(vector<string>& vector) {
    vector.clear();
    vector.insert(vector.end(), cmds.begin(), cmds.end());
    for (auto& cmd : vector) {
        if (cmd.find(' ') != string::npos) {
            cmd = "\"" + cmd + "\"";
        }
    }
}

int CommandLine::GetParameterFromList(const char* const params[], vector<string>& output) {
    output.clear();
    enum { P_START, P_FIRSTNOMATCH, P_FIRSTMATCH } state = P_START;
    int retval = 1;
    for (auto it = cmds.begin(); it != cmds.end(); ) {
        bool found = false;
        int param_idx = 0;
        for (size_t i = 0; params[i][0]; ++i, ++param_idx) {
            if (strcasecmp(it->c_str(), params[i]) == 0) {
                found = true;
                switch (state) {
                    case P_START:
                        retval = static_cast<int>(i + 2);
                        state = P_FIRSTMATCH;
                        break;
                    case P_FIRSTMATCH:
                    case P_FIRSTNOMATCH:
                        return retval;
                }
                break;
            }
        }
        auto it_old = it++;
        if (!found) {
            switch (state) {
                case P_START:
                    retval = 0;
                    state = P_FIRSTNOMATCH;
                    output.push_back(*it_old);
                    break;
                case P_FIRSTMATCH:
                case P_FIRSTNOMATCH:
                    output.push_back(*it_old);
                    break;
            }
        }
        cmds.erase(it_old);
    }
    return retval;
}

CommandLine::CommandLine(int argc, const char* const argv[]) {
    if (argc > 0) {
        file_name = argv[0];
    }
    cmds.assign(argv + 1, argv + argc);
}

Bit16u CommandLine::Get_arglength() {
    if (cmds.empty()) {
        return 0;
    }
    size_t len = cmds.front().length();
    auto it = cmds.begin();
    ++it;
    for (; it != cmds.end(); ++it) {
        len += it->length() + 1;
    }
    return static_cast<Bit16u>(len);
}

CommandLine::CommandLine(char const* const name, char const* const cmdline) {
    if (name) {
        file_name = name;
    }
    string cmdline_str = cmdline ? cmdline : "";
    bool inword = false, inquote = false;
    string str;
    for (size_t i = 0; i < cmdline_str.length(); ++i) {
        char c = cmdline_str[i];
        if (inquote) {
            if (c != '"') {
                str += c;
            } else {
                inquote = false;
                if (!str.empty()) {
                    cmds.push_back(str);
                    str.clear();
                }
            }
        } else if (inword) {
            if (c != ' ') {
                str += c;
            } else {
                inword = false;
                if (!str.empty()) {
                    cmds.push_back(str);
                    str.clear();
                }
            }
        } else if (c == '"') {
            inquote = true;
        } else if (c != ' ') {
            str += c;
            inword = true;
        }
    }
    if (!str.empty()) {
        cmds.push_back(str);
    }
}

void CommandLine::Shift(unsigned int amount) {
    while (amount--) {
        file_name = cmds.empty() ? "" : cmds.front();
        if (!cmds.empty()) {
            cmds.pop_front();
        }
    }
}