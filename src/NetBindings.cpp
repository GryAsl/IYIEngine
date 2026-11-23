// Exposes multiplayer (libraries/net, the IYI::Api Net functions) to Python as engine.net (SPEC §25.5).
// Built only with IYI_NET. A project that does not list libraries/net gets a stub whose every name
// raises ImportError, so a game cannot use multiplayer in development and lose it when packaged.
//
// Connected to:
// - libraries/net/src/Net.h (the C++ it calls); registered by pybind11_init_engine in PythonRuntime.cpp.

#include "net/src/Net.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

namespace IYI {

namespace {

struct PyNetObject { int id = 0; };

const char* TypeName(NetType t)
{
    switch (t) {
    case NetType::Bool: return "a bool";
    case NetType::F32: case NetType::Fixed: case NetType::Angle: return "a number";
    case NetType::Vec2: return "(x, y) or an object with x, y";
    case NetType::Vec3: return "(x, y, z) or an object with x, y, z";
    case NetType::Text: return "a str";
    case NetType::Bytes: return "bytes";
    default: return "an int";
    }
}

NetValue ToValue(const NetFieldType& t, const py::handle& v, const std::string& name)
{
    try {
        switch (t.type) {
        case NetType::Bool: return v.cast<bool>();
        case NetType::F32: case NetType::Fixed: case NetType::Angle: return v.cast<double>();
        case NetType::Vec2:
            if (py::hasattr(v, "x")) return Vector2{ v.attr("x").cast<float>(), v.attr("y").cast<float>() };
            else {
                auto s = v.cast<py::sequence>();
                if (py::len(s) == 2) return Vector2{ s[0].cast<float>(), s[1].cast<float>() };
            }
            break;
        case NetType::Vec3:
            if (py::hasattr(v, "x")) return Vector3{ v.attr("x").cast<float>(), v.attr("y").cast<float>(), v.attr("z").cast<float>() };
            else {
                auto s = v.cast<py::sequence>();
                if (py::len(s) == 3) return Vector3{ s[0].cast<float>(), s[1].cast<float>(), s[2].cast<float>() };
            }
            break;
        case NetType::Text: return v.cast<std::string>();
        case NetType::Bytes:
            if (py::isinstance<py::bytes>(v)) return static_cast<std::string>(v.cast<py::bytes>());
            break;
        default: return v.cast<int64_t>();
        }
    } catch (const py::cast_error&) {
    }
    throw py::type_error("net field '" + name + "' expects " + TypeName(t.type));
}

py::object FromValue(const NetFieldType& t, const NetValue& v)
{
    if (const bool* b = std::get_if<bool>(&v)) return py::bool_(*b);
    if (const int64_t* i = std::get_if<int64_t>(&v)) return py::int_(*i);
    if (const double* d = std::get_if<double>(&v)) return py::float_(*d);
    if (const Vector2* p = std::get_if<Vector2>(&v)) return py::make_tuple(p->x, p->y);
    if (const Vector3* p = std::get_if<Vector3>(&v)) return py::make_tuple(p->x, p->y, p->z);
    const std::string& s = std::get<std::string>(v);
    if (t.type == NetType::Bytes) return py::bytes(s);
    return py::str(s);
}

template <class Lookup>
NetValues FromKwargs(const py::kwargs& kwargs, Lookup lookup, const std::string& what)
{
    NetValues values;
    for (auto item : kwargs) {
        const std::string name = item.first.cast<std::string>();
        std::optional<NetFieldType> t = lookup(name);
        if (!t) throw py::type_error(what + " has no field '" + name + "'");
        values.emplace_back(name, ToValue(*t, item.second, name));
    }
    return values;
}

std::vector<NetField> FieldsFrom(const py::kwargs& kwargs)
{
    std::vector<NetField> fields;
    for (auto item : kwargs) {
        if (!py::isinstance<NetFieldType>(item.second)) {
            throw py::type_error("field '" + item.first.cast<std::string>() + "' needs a net type (net.u8, net.fixed(...), ...)");
        }
        fields.push_back({ item.first.cast<std::string>(), item.second.cast<NetFieldType>() });
    }
    return fields;
}

template <class Lookup>
py::dict ValuesDict(const NetValues& values, Lookup lookup)
{
    py::dict d;
    for (const auto& [name, value] : values) {
        if (std::optional<NetFieldType> t = lookup(name)) d[py::str(name)] = FromValue(*t, value);
    }
    return d;
}

py::object ObjectOrNone(int id)
{
    if (id == 0 || !Api::NetExists(id)) return py::none();
    return py::cast(PyNetObject{ id });
}

std::optional<NetFieldType> FieldOf(const PyNetObject& o, const std::string& name)
{
    const std::string kind = Api::NetObjectKind(o.id);
    if (kind.empty()) throw py::attribute_error("net object " + std::to_string(o.id) + " no longer exists");
    return Api::NetKindField(kind, name);
}

} // namespace

void RegisterNetBindings(py::module_& m)
{
    py::module_ net = m.def_submodule("net", "IYI multiplayer (libraries/net, SPEC §25)");
    if (!NetListed()) {
        net.def("__getattr__", [](const std::string& name) -> py::object {
            if (name.rfind("__", 0) == 0) throw py::attribute_error(name);
            throw py::import_error("engine.net needs \"../IYIEngine/libraries/net\" in project.json \"libraries\" (SPEC §25.2)");
        });
        return;
    }

    net.attr("HOST") = kNetHost;
    net.attr("ALL") = kNetAll;
    net.attr("OTHERS") = kNetOthers;

    py::class_<NetFieldType>(net, "FieldType")
        .def("__repr__", [](const NetFieldType& f) { return "<net field type " + std::to_string(static_cast<int>(f.type)) + ">"; });
    const std::pair<const char*, NetType> simple[] = {
        { "bool", NetType::Bool }, { "u8", NetType::U8 }, { "u16", NetType::U16 }, { "u32", NetType::U32 },
        { "i8", NetType::I8 }, { "i16", NetType::I16 }, { "i32", NetType::I32 }, { "f32", NetType::F32 },
        { "player", NetType::Player }, { "ref", NetType::Ref },
    };
    for (const auto& [name, type] : simple) net.attr(name) = NetSimple(type);
    net.def("fixed", &NetFixed, py::arg("min"), py::arg("max"), py::arg("step"));
    net.def("angle", &NetAngle, py::arg("bits") = 12);
    net.def("vec2", &NetVec2, py::arg("min"), py::arg("max"), py::arg("step"));
    net.def("vec3", &NetVec3, py::arg("min"), py::arg("max"), py::arg("step"));
    net.def("text", &NetText, py::arg("max_bytes"));
    net.def("bytes", &NetBytes, py::arg("max_bytes"));

    // --- session ---
    net.def("host", [](const std::string& transport, int port, int maxPlayers, int tickRate, const std::string& name, bool isPublic,
                       const py::dict& data, const std::string& playerName, float timeout, float migrationTimeout) {
        NetHostOptions o;
        o.transport = transport;
        o.port = port;
        o.maxPlayers = maxPlayers;
        o.tickRate = tickRate;
        o.name = name;
        o.isPublic = isPublic;
        for (auto item : data) o.data.emplace_back(py::str(item.first).cast<std::string>(), py::str(item.second).cast<std::string>());
        o.playerName = playerName;
        o.timeout = timeout;
        o.migrationTimeout = migrationTimeout;
        return Api::NetHost(o);
    }, py::arg("transport") = "enet", py::arg("port") = 7777, py::arg("max_players") = 8, py::arg("tick_rate") = 30,
       py::arg("name") = "", py::arg("public") = true, py::arg("data") = py::dict(), py::arg("player_name") = "",
       py::arg("timeout") = 10.0f, py::arg("migration_timeout") = 3.0f);
    net.def("join", [](const py::object& target, const std::string& playerName) {
        const std::string t = py::isinstance<py::dict>(target) ? target["target"].cast<std::string>() : target.cast<std::string>();
        return Api::NetJoin(t, playerName);
    }, py::arg("target"), py::arg("player_name") = "");
    net.def("find", &Api::NetFind, py::arg("transport") = "enet");
    net.def("leave", &Api::NetLeave);
    net.def("simulate", &Api::NetSimulate, py::arg("latency_ms") = 0.0f, py::arg("jitter_ms") = 0.0f, py::arg("loss") = 0.0f);
    net.def("state", &Api::NetState);
    net.def("is_host", &Api::NetIsHost);
    net.def("local_player", &Api::NetLocalPlayer);
    net.def("tick", &Api::NetTick);
    net.def("set_lobby_data", &Api::NetSetLobbyData, py::arg("key"), py::arg("value"));
    net.def("players", [] {
        py::list out;
        for (const NetPlayer& p : Api::NetPlayers()) {
            py::dict d;
            d["id"] = p.id;
            d["name"] = p.name;
            d["rtt_ms"] = p.rttMs;
            d["loss"] = p.loss;
            d["is_host"] = p.isHost;
            out.append(d);
        }
        return out;
    });
    net.def("stats", [] {
        const NetStats s = Api::NetGetStats();
        py::dict d;
        d["bytes_in_per_sec"] = s.bytesInPerSec;
        d["bytes_out_per_sec"] = s.bytesOutPerSec;
        d["packets_in"] = s.packetsIn;
        d["packets_out"] = s.packetsOut;
        d["dropped"] = s.dropped;
        d["last_snapshot_bytes"] = s.lastSnapshotBytes;
        d["objects"] = s.objects;
        return d;
    });
    net.def("events", [] {
        py::list out;
        for (const NetEvent& e : Api::NetEvents()) {
            py::dict d;
            d["type"] = e.type;
            if (e.player) d["player"] = e.player;
            if (!e.name.empty()) d["name"] = e.name;
            if (!e.reason.empty()) d["reason"] = e.reason;
            if (!e.target.empty()) {
                d["target"] = e.target;
                d["players"] = e.players;
                d["max_players"] = e.maxPlayers;
                py::dict data;
                for (const auto& [k, v] : e.data) data[py::str(k)] = v;
                d["data"] = data;
            }
            out.append(d);
        }
        return out;
    });

    // --- replicated objects ---
    net.def("define", [](const std::string& kind, const py::kwargs& fields) {
        if (!Api::NetDefine(kind, FieldsFrom(fields))) throw py::value_error("net.define('" + kind + "') failed (see the log)");
    }, py::arg("kind"));
    py::class_<PyNetObject>(net, "Object")
        .def_property_readonly("id", [](const PyNetObject& o) { return o.id; })
        .def_property_readonly("kind", [](const PyNetObject& o) { return Api::NetObjectKind(o.id); })
        .def_property_readonly("owner", [](const PyNetObject& o) { return Api::NetObjectOwner(o.id); })
        .def_property_readonly("authority", [](const PyNetObject& o) { return Api::NetObjectOwnerAuthority(o.id) ? "owner" : "host"; })
        .def_property_readonly("exists", [](const PyNetObject& o) { return Api::NetExists(o.id); })
        .def("__getattr__", [](const PyNetObject& o, const std::string& name) -> py::object {
            std::optional<NetFieldType> t = FieldOf(o, name);
            if (!t) throw py::attribute_error("net object has no field '" + name + "'");
            return FromValue(*t, *Api::NetGet(o.id, name));
        })
        .def("__setattr__", [](const PyNetObject& o, const std::string& name, const py::object& value) {
            if (name == "priority") {
                Api::NetSetPriority(o.id, value.cast<float>());
                return;
            }
            std::optional<NetFieldType> t = FieldOf(o, name);
            if (!t) throw py::attribute_error("net object has no field '" + name + "' (id, kind, owner and authority are read-only)");
            const NetValue v = ToValue(*t, value, name);
            if (!Api::NetCanWrite(o.id)) throw py::value_error("no authority to write '" + name + "' (host objects: the host; authority=\"owner\": its owner)");
            Api::NetSet(o.id, name, v);
        })
        .def("sample", [](const PyNetObject& o, const std::string& name) -> py::object {
            std::optional<NetFieldType> t = FieldOf(o, name);
            if (!t) throw py::attribute_error("net object has no field '" + name + "'");
            return FromValue(*t, *Api::NetSample(o.id, name));
        }, py::arg("field"))
        .def("hide_from", [](const PyNetObject& o, int player) { Api::NetSetVisible(o.id, player, false); }, py::arg("player"))
        .def("show_to", [](const PyNetObject& o, int player) { Api::NetSetVisible(o.id, player, true); }, py::arg("player"))
        .def("despawn", [](const PyNetObject& o) { return Api::NetDespawn(o.id); })
        .def("__eq__", [](const PyNetObject& a, const py::object& b) { return py::isinstance<PyNetObject>(b) && b.cast<PyNetObject>().id == a.id; })
        .def("__hash__", [](const PyNetObject& o) { return o.id; })
        .def("__repr__", [](const PyNetObject& o) { return "<net.Object " + std::to_string(o.id) + " " + Api::NetObjectKind(o.id) + ">"; });
    net.def("spawn", [](const std::string& kind, int owner, const std::string& authority, float priority, const py::kwargs& values) {
        if (authority != "host" && authority != "owner") throw py::value_error("authority is \"host\" or \"owner\"");
        const NetValues v = FromKwargs(values, [&](const std::string& f) { return Api::NetKindField(kind, f); }, "kind '" + kind + "'");
        return ObjectOrNone(Api::NetSpawn(kind, owner, authority == "owner", priority, v));
    }, py::arg("kind"), py::arg("owner") = 0, py::arg("authority") = "host", py::arg("priority") = 1.0f);
    net.def("objects", [](const py::object& kind) {
        py::list out;
        for (int id : Api::NetObjects(kind.is_none() ? std::string() : kind.cast<std::string>())) out.append(PyNetObject{ id });
        return out;
    }, py::arg("kind") = py::none());
    net.def("get", &ObjectOrNone, py::arg("object_id"));

    // --- messages and inputs ---
    net.def("message", [](const std::string& name, const py::kwargs& fields) {
        if (!Api::NetDefineMessage(name, FieldsFrom(fields))) throw py::value_error("net.message('" + name + "') failed (see the log)");
    }, py::arg("name"));
    net.def("send", [](const std::string& name, int to, bool reliable, const py::kwargs& values) {
        const NetValues v = FromKwargs(values, [&](const std::string& f) { return Api::NetMessageField(name, f); }, "message '" + name + "'");
        return Api::NetSend(name, to, reliable, v);
    }, py::arg("name"), py::arg("to") = kNetHost, py::arg("reliable") = true);
    net.def("receive", [] {
        py::list out;
        for (const NetMessage& msg : Api::NetReceive()) {
            py::dict d = ValuesDict(msg.values, [&](const std::string& f) { return Api::NetMessageField(msg.name, f); });
            d["name"] = msg.name;
            d["from"] = msg.from;
            d["tick"] = msg.tick;
            out.append(d);
        }
        return out;
    });
    net.def("define_input", [](const py::kwargs& fields) {
        if (!Api::NetDefineInput(FieldsFrom(fields))) throw py::value_error("net.define_input failed (see the log)");
    });
    net.def("send_input", [](const py::kwargs& values) {
        return Api::NetSendInput(FromKwargs(values, [](const std::string& f) { return Api::NetInputField(f); }, "the input"));
    });
    net.def("inputs", [](int player) {
        py::list out;
        for (const NetInput& in : Api::NetInputs(player)) {
            py::dict d = ValuesDict(in.values, [](const std::string& f) { return Api::NetInputField(f); });
            d["seq"] = in.seq;
            d["tick"] = in.tick;
            out.append(d);
        }
        return out;
    }, py::arg("player"));
    net.def("input_ack", &Api::NetInputAck);
    net.def("rewind", [](uint32_t tick) {
        py::dict out;
        for (const auto& [id, values] : Api::NetRewind(tick)) {
            const std::string kind = Api::NetObjectKind(id);
            out[py::int_(id)] = ValuesDict(values, [&](const std::string& f) { return Api::NetKindField(kind, f); });
        }
        return out;
    }, py::arg("tick"));
}

} // namespace IYI
