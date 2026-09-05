// MuCoTo main app: REST API over the audio and midi engines, static web ui.
#include <filesystem>
#include <fstream>
#include <sstream>

#include "rest.hh"
#include "audio.hh"
#include "midi.hh"
#include "b64.hh"

// ---------- application singleton ----------

static std::string firstExisting(char const* env, std::vector<std::string> const& candidates)
{
    if (auto v = getenv(env))
        return v;
    for (auto const& c: candidates)
        if (std::filesystem::exists(c))
            return c;
    return candidates[0];
}
struct App
{
    audio::Engine audio;
    midi::MidiEngine midi{audio};
    std::string webRoot = firstExisting("MUCOTO_WEB", {"web/dist", "../web/dist"});
    std::string specFile = firstExisting("MUCOTO_SPECS", {"node_specs.json", "src/node_specs.json"});
    std::string stateFile = getenv("MUCOTO_STATE") ? getenv("MUCOTO_STATE") : "mucoto.json";
};
App& app()
{
    static App a;
    return a;
}

// ---------- serializable state ----------

struct NodeState
{
    std::string name;
    std::string type;
    std::map<std::string, float> params;
    std::map<std::string, std::string> sparams;
    int outputChannels = 1; // filled in api replies, ignored on load
};
struct LinkState
{
    std::string from;
    int fromChannel = 0;
    std::string to;
    int toInput = 0;
};
struct MidiEventState
{
    double t = 0;
    int type = 0;
    int channel = 0;
    int d1 = 0;
    int d2 = 0;
};
struct ClipState
{
    std::vector<MidiEventState> events;
    double length = 0;
};
struct PadState
{
    int note = 0;
    int target = 0;
    int instrument = -1;
    int state = 0; // runtime, api replies only
    int color = 0; // runtime, api replies only (launchpad palette entry)
    std::string recorder;
    std::string player;
    std::string sample;
    ClipState clip;
};
struct InputState
{
    int id = 0;
    std::string port;
    int looper = 0;
    int defaultRoute = -1;
    int connected = 0; // runtime
    std::map<std::string, int> channelRoutes;
    std::vector<PadState> pads;
};
struct OutputState
{
    int id = 0;
    std::string port;
    int connected = 0; // runtime
};
struct BindingState
{
    int channel = 0;
    int cc = 0;
    std::string action;
    std::string node;
    std::string param;
    int input = -1;
    int output = -1;
    float min = 0;
    float max = 1;
};
struct SampleState
{
    std::string name;
    std::string data; // base64 of raw float32
};
struct SaveFile: public jsonser::JsonSerializable
{
    double bpm = 120;
    std::vector<NodeState> nodes;
    std::vector<LinkState> links;
    std::vector<SampleState> samples;
    std::vector<InputState> inputs;
    std::vector<OutputState> outputs;
    std::vector<BindingState> bindings;
};

// ---------- conversions ----------

static ClipState toState(midi::Clip const& c)
{
    ClipState res;
    res.length = c.length;
    for (auto const& e: c.events)
        res.events.push_back(MidiEventState{e.t, e.type, e.channel, e.d1, e.d2});
    return res;
}
static midi::Clip fromState(ClipState const& c)
{
    midi::Clip res;
    res.length = c.length;
    for (auto const& e: c.events)
        res.events.push_back(midi::MidiEvent{e.t, e.type, e.channel, e.d1, e.d2});
    return res;
}
// mirror of MidiEngine::padColor so the web looper panel matches the leds
static int padColorOf(midi::Pad const& pad, std::map<int, midi::OutputCfg> const& outputs)
{
    static const int palette[] = {21, 45, 13, 37, 53, 9, 41, 17};
    static const int n = sizeof(palette) / sizeof(palette[0]);
    if (pad.target == 1)
    {
        int idx = 0;
        for (auto const& [id, out]: outputs)
        {
            if (id == pad.instrument)
                break;
            idx++;
        }
        return palette[idx % n];
    }
    if (pad.target == 2)
        return palette[std::hash<std::string>()(
            pad.player.empty() ? pad.recorder : pad.player) % n];
    return 0;
}
static InputState toState(midi::InputCfg const& in,
                          std::map<int, midi::OutputCfg> const& outputs)
{
    InputState res;
    res.id = in.id;
    res.port = in.port;
    res.looper = in.looper ? 1 : 0;
    res.defaultRoute = in.defaultRoute;
    res.connected = in.connected ? 1 : 0;
    for (auto const& [ch, out]: in.channelRoutes)
        res.channelRoutes[std::to_string(ch)] = out;
    for (auto const& [note, pad]: in.pads)
    {
        PadState ps;
        ps.note = note;
        ps.target = pad.target;
        ps.instrument = pad.instrument;
        ps.state = (int)pad.st;
        ps.color = padColorOf(pad, outputs);
        ps.recorder = pad.recorder;
        ps.player = pad.player;
        ps.sample = pad.sample;
        ps.clip = toState(pad.clip);
        res.pads.push_back(std::move(ps));
    }
    return res;
}
static midi::InputCfg fromState(InputState const& in)
{
    midi::InputCfg res;
    res.id = in.id;
    res.port = in.port;
    res.looper = in.looper;
    res.defaultRoute = in.defaultRoute;
    for (auto const& [ch, out]: in.channelRoutes)
        res.channelRoutes[std::stoi(ch)] = out;
    for (auto const& ps: in.pads)
    {
        midi::Pad pad;
        pad.target = ps.target;
        pad.instrument = ps.instrument;
        pad.recorder = ps.recorder;
        pad.player = ps.player;
        pad.sample = ps.sample;
        pad.clip = fromState(ps.clip);
        pad.st = pad.target ? midi::Pad::St::Idle : midi::Pad::St::Empty;
        res.pads[ps.note] = std::move(pad);
    }
    return res;
}

static SaveFile captureState()
{
    auto& a = app();
    SaveFile s;
    s.bpm = a.midi.bpm();
    for (auto const& n: a.audio.graph().nodes())
        s.nodes.push_back(NodeState{n->name(), n->type(), n->params(),
                                    n->stringParams(), n->outputChannels()});
    for (auto const& l: a.audio.graph().links())
        s.links.push_back(LinkState{l.from, l.fromChannel, l.to, l.toInput});
    for (auto const& [name, buf]: a.audio.bank().all())
        s.samples.push_back(SampleState{
            name, b64::encode(buf->data(), buf->size() * sizeof(float))});
    auto outputs = a.midi.outputs();
    for (auto const& [id, in]: a.midi.inputs())
        s.inputs.push_back(toState(in, outputs));
    for (auto const& [id, out]: outputs)
        s.outputs.push_back(OutputState{out.id, out.port, out.connected ? 1 : 0});
    for (auto const& b: a.midi.bindings())
        s.bindings.push_back(BindingState{b.channel, b.cc, b.target.action,
                                          b.target.node, b.target.param, b.target.input,
                                          b.target.output, b.target.min, b.target.max});
    return s;
}
static void applyState(SaveFile const& s)
{
    auto& a = app();
    a.audio.graph().clear();
    for (auto const& ns: s.nodes)
    {
        auto node = a.audio.createNode(ns.type, ns.name);
        for (auto const& [k, v]: ns.params)
            node->setParam(k, v);
        for (auto const& [k, v]: ns.sparams)
            node->setStringParam(k, v);
        a.audio.graph().addNode(node);
    }
    for (auto const& ls: s.links)
        try
        {
            a.audio.graph().addLink(audio::Link{ls.from, ls.fromChannel, ls.to, ls.toInput});
        }
        catch (std::exception const& e)
        {
            std::cerr << "link " << ls.from << "->" << ls.to << ": " << e.what() << std::endl;
        }
    for (auto const& name: a.audio.bank().names())
        a.audio.bank().remove(name);
    for (auto const& ss: s.samples)
    {
        auto raw = b64::decode(ss.data);
        auto buf = std::make_shared<audio::Buffer>(raw.size() / sizeof(float));
        memcpy(buf->data(), raw.data(), buf->size() * sizeof(float));
        a.audio.bank().set(ss.name, buf);
    }
    std::map<int, midi::InputCfg> inputs;
    for (auto const& is: s.inputs)
        inputs[is.id] = fromState(is);
    std::map<int, midi::OutputCfg> outputs;
    for (auto const& os: s.outputs)
    {
        midi::OutputCfg o;
        o.id = os.id;
        o.port = os.port;
        outputs[o.id] = o;
    }
    std::vector<midi::Binding> bindings;
    for (auto const& bs: s.bindings)
        bindings.push_back(midi::Binding{bs.channel, bs.cc,
            midi::LearnTarget{bs.action, bs.node, bs.param, bs.input, bs.output,
                              bs.min, bs.max}});
    a.midi.restore(inputs, outputs, bindings, s.bpm);
}

// ---------- api request/reply payloads ----------

struct StatusResp
{
    double bpm = 120;
    int rolling = 0;
    double measures = 0;
    int learning = 0;
    int rate = 48000;
    int padInput = -1;
    int padNote = -1;
};
struct GraphResp
{
    std::vector<NodeState> nodes;
    std::vector<LinkState> links;
};
struct PortResp
{
    std::string name;
    int input = 0;
    int output = 0;
};
struct MidiConfigResp
{
    std::vector<InputState> inputs;
    std::vector<OutputState> outputs;
    std::vector<BindingState> bindings;
};
struct SampleInfo
{
    std::string name;
    int frames = 0;
};
struct NewNodeReq
{
    std::string type;
    std::string name;
};
struct ParamReq
{
    std::string name;
    float value = 0;
};
struct SParamReq
{
    std::string name;
    std::string value;
};
struct PortReq
{
    std::string port;
};
struct RouteReq
{
    int channel = -1; ///< -1: default route
    int output = -1;
};
struct IntReq
{
    int value = 0;
};
struct PlayReq
{
    std::string sample;
    int loop = 0;
};
struct NameReq
{
    std::string name;
};
struct CutReq
{
    int start = 0;
    int end = 0;
};
struct FileReq
{
    std::string file;
};
struct IdResp
{
    int id = -1;
};

// ---------- api ----------

static std::string mimeType(std::string const& path)
{
    auto dot = path.rfind('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    if (ext == "html")
        return "text/html";
    if (ext == "js")
        return "application/javascript";
    if (ext == "css")
        return "text/css";
    if (ext == "json")
        return "application/json";
    if (ext == "svg")
        return "image/svg+xml";
    if (ext == "png")
        return "image/png";
    if (ext == "ico")
        return "image/x-icon";
    return "application/octet-stream";
}

class Api: public Rest::RouteHandler
{
public:
    // ----- status / transport -----
    [[=Rest::route("/api/status")]]
    StatusResp Status()
    {
        auto& a = app();
        auto [pi, pn] = a.midi.currentPad();
        return StatusResp{a.midi.bpm(), a.midi.rolling() ? 1 : 0, a.midi.measures(),
                          a.midi.learning() ? 1 : 0, a.audio.rate(), pi, pn};
    }
    [[=Rest::route("/api/bpm/{value}")]]
    void SetBpm(double const& value)
    {
        app().midi.setBpm(value);
    }

    // ----- node specs -----
    [[=Rest::route("/api/specs")]]
    void Specs()
    {
        std::ifstream f(app().specFile);
        std::stringstream ss;
        ss << f.rdbuf();
        response().headers["Content-Type"] = "application/json";
        response().payload = cppgow::BufferView::copy(ss.str());
    }

    // ----- audio graph -----
    [[=Rest::route("/api/graph")]]
    GraphResp Graph()
    {
        GraphResp res;
        for (auto const& n: app().audio.graph().nodes())
            res.nodes.push_back(NodeState{n->name(), n->type(), n->params(),
                                          n->stringParams(), n->outputChannels()});
        for (auto const& l: app().audio.graph().links())
            res.links.push_back(LinkState{l.from, l.fromChannel, l.to, l.toInput});
        return res;
    }
    [[=Rest::route("/api/graph/node")]]
    void AddNode(NewNodeReq const& req)
    {
        auto node = app().audio.createNode(req.type, req.name);
        app().audio.graph().addNode(node);
    }
    [[=Rest::route("/api/graph/node/{name}/delete")]]
    void DeleteNode(std::string const& name)
    {
        app().audio.graph().removeNode(name);
    }
    [[=Rest::route("/api/graph/link")]]
    void AddLink(LinkState const& req)
    {
        app().audio.graph().addLink(
            audio::Link{req.from, req.fromChannel, req.to, req.toInput});
    }
    [[=Rest::route("/api/graph/link/delete")]]
    void DeleteLink(LinkState const& req)
    {
        app().audio.graph().removeLink(
            audio::Link{req.from, req.fromChannel, req.to, req.toInput});
    }
    [[=Rest::route("/api/node/{name}/param")]]
    void SetParam(std::string const& name, ParamReq const& req)
    {
        auto node = app().audio.graph().node(name);
        if (!node)
            throw std::runtime_error("no such node");
        node->setParam(req.name, req.value);
    }
    [[=Rest::route("/api/node/{name}/sparam")]]
    void SetSParam(std::string const& name, SParamReq const& req)
    {
        auto node = app().audio.graph().node(name);
        if (!node)
            throw std::runtime_error("no such node");
        node->setStringParam(req.name, req.value);
    }

    // ----- players / recorders (ui triggered) -----
    [[=Rest::route("/api/player/{name}/play")]]
    void PlayerPlay(std::string const& name, PlayReq const& req)
    {
        auto p = dynamic_cast<audio::SoundBankPlayerNode*>(
            app().audio.graph().node(name).get());
        if (!p)
            throw std::runtime_error("no such player");
        p->play(req.sample, req.loop);
    }
    [[=Rest::route("/api/player/{name}/stop")]]
    void PlayerStop(std::string const& name)
    {
        auto p = dynamic_cast<audio::SoundBankPlayerNode*>(
            app().audio.graph().node(name).get());
        if (p)
            p->stop();
    }
    [[=Rest::route("/api/recorder/{name}/take")]]
    void RecorderTake(std::string const& name, NameReq const& req)
    {
        auto r = dynamic_cast<audio::RecorderNode*>(app().audio.graph().node(name).get());
        if (!r)
            throw std::runtime_error("no such recorder");
        auto data = r->take();
        if (data && !data->empty())
            app().audio.bank().set(req.name, data);
    }

    // ----- sample bank -----
    [[=Rest::route("/api/samples")]]
    std::vector<SampleInfo> Samples()
    {
        std::vector<SampleInfo> res;
        for (auto const& [name, buf]: app().audio.bank().all())
            res.push_back(SampleInfo{name, (int)buf->size()});
        return res;
    }
    [[=Rest::route("/api/sample/{name}")]]
    void Sample(std::string const& name)
    {
        auto buf = app().audio.bank().get(name);
        if (!buf)
            throw std::runtime_error("no such sample");
        response().headers["Content-Type"] = "application/octet-stream";
        response().payload = cppgow::BufferView::copy(
            std::string((char const*)buf->data(), buf->size() * sizeof(float)));
    }
    [[=Rest::route("/api/sample/{name}/cut")]]
    void SampleCut(std::string const& name, CutReq const& req)
    {
        auto buf = app().audio.bank().get(name);
        if (!buf)
            throw std::runtime_error("no such sample");
        int start = std::clamp(req.start, 0, (int)buf->size());
        int end = std::clamp(req.end, start, (int)buf->size());
        app().audio.bank().set(name, std::make_shared<audio::Buffer>(
            buf->begin() + start, buf->begin() + end));
    }
    [[=Rest::route("/api/sample/{name}/delete")]]
    void SampleDelete(std::string const& name)
    {
        app().audio.bank().remove(name);
    }

    // ----- midi -----
    [[=Rest::route("/api/midi/ports")]]
    std::vector<PortResp> Ports()
    {
        std::vector<PortResp> res;
        for (auto const& p: app().midi.ports())
            res.push_back(PortResp{p.name, p.isInput ? 1 : 0, p.isOutput ? 1 : 0});
        return res;
    }
    [[=Rest::route("/api/midi/config")]]
    MidiConfigResp MidiConfig()
    {
        MidiConfigResp res;
        auto outputs = app().midi.outputs();
        for (auto const& [id, in]: app().midi.inputs())
            res.inputs.push_back(toState(in, outputs));
        for (auto const& [id, out]: outputs)
            res.outputs.push_back(OutputState{out.id, out.port, out.connected ? 1 : 0});
        for (auto const& b: app().midi.bindings())
            res.bindings.push_back(BindingState{b.channel, b.cc, b.target.action,
                b.target.node, b.target.param, b.target.input, b.target.output,
                b.target.min, b.target.max});
        return res;
    }
    [[=Rest::route("/api/midi/input")]]
    IdResp AddInput(PortReq const& req)
    {
        return IdResp{app().midi.addInput(req.port)};
    }
    [[=Rest::route("/api/midi/input/{id}/delete")]]
    void DeleteInput(int const& id)
    {
        app().midi.removeInput(id);
    }
    [[=Rest::route("/api/midi/input/{id}/looper")]]
    void SetLooper(int const& id, IntReq const& req)
    {
        app().midi.setLooper(id, req.value);
    }
    [[=Rest::route("/api/midi/input/{id}/route")]]
    void SetRoute(int const& id, RouteReq const& req)
    {
        if (req.channel < 0)
            app().midi.setDefaultRoute(id, req.output);
        else
            app().midi.setChannelRoute(id, req.channel, req.output);
    }
    [[=Rest::route("/api/midi/output")]]
    IdResp AddOutput(PortReq const& req)
    {
        return IdResp{app().midi.addOutput(req.port)};
    }
    [[=Rest::route("/api/midi/output/{id}/delete")]]
    void DeleteOutput(int const& id)
    {
        app().midi.removeOutput(id);
    }
    [[=Rest::route("/api/midi/rescan")]]
    void Rescan()
    {
        app().midi.rescan();
    }

    // ----- looper pads / clips -----
    [[=Rest::route("/api/clip/{input}/{note}")]]
    ClipState GetClip(int const& input, int const& note)
    {
        return toState(app().midi.clipOf(input, note));
    }
    [[=Rest::route("/api/clip/{input}/{note}/set")]]
    void SetClip(int const& input, int const& note, ClipState const& clip)
    {
        app().midi.setClip(input, note, fromState(clip));
    }
    [[=Rest::route("/api/pad/{input}/{note}/clear")]]
    void ClearPad(int const& input, int const& note)
    {
        app().midi.clearPad(input, note);
    }

    // ----- midi learn -----
    [[=Rest::route("/api/learn")]]
    void Learn(BindingState const& req)
    {
        app().midi.learn(midi::LearnTarget{req.action, req.node, req.param,
                                           req.input, req.output, req.min, req.max});
    }
    [[=Rest::route("/api/learn/cancel")]]
    void LearnCancel()
    {
        app().midi.cancelLearn();
    }
    [[=Rest::route("/api/binding/{index}/delete")]]
    void DeleteBinding(int const& index)
    {
        app().midi.removeBinding(index);
    }

    // ----- save / load -----
    [[=Rest::route("/api/save")]]
    void Save(FileReq const& req)
    {
        auto file = req.file.empty() ? app().stateFile : req.file;
        std::ofstream f(file);
        f << captureState().toJson();
        if (!f)
            throw std::runtime_error("cannot write " + file);
    }
    [[=Rest::route("/api/load")]]
    void Load(FileReq const& req)
    {
        auto file = req.file.empty() ? app().stateFile : req.file;
        std::ifstream f(file);
        if (!f)
            throw std::runtime_error("cannot read " + file);
        std::stringstream ss;
        ss << f.rdbuf();
        SaveFile s;
        s.fromJson(ss.str());
        applyState(s);
    }

    // ----- static web ui -----
    [[=Rest::route("/")]]
    void Static()
    {
        auto path = request().path;
        if (path.find("..") != std::string::npos)
            throw std::runtime_error("bad path");
        if (path.empty() || path == "/")
            path = "/index.html";
        auto full = app().webRoot + path;
        std::ifstream f(full, std::ios::binary);
        if (!f)
        { // spa fallback
            full = app().webRoot + "/index.html";
            f = std::ifstream(full, std::ios::binary);
            path = "/index.html";
        }
        if (!f)
        {
            response().statusCode = 404;
            return;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        response().headers["Content-Type"] = mimeType(path);
        response().payload = cppgow::BufferView::copy(ss.str());
    }
};

int main(int argc, char** argv)
{
    std::string listen = ":8000";
    bool midiDebug = false;
    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg == "-d" || arg == "--midi-debug")
            midiDebug = true;
        else if (arg == "-h" || arg == "--help")
        {
            std::cout << "usage: mucoto [listen_address] [-d|--midi-debug]" << std::endl;
            return 0;
        }
        else
            listen = arg;
    }
    auto& a = app();
    a.midi.setDebug(midiDebug);
    if (std::filesystem::exists(a.stateFile))
    {
        try
        {
            std::ifstream f(a.stateFile);
            std::stringstream ss;
            ss << f.rdbuf();
            SaveFile s;
            s.fromJson(ss.str());
            applyState(s);
            std::cout << "state loaded from " << a.stateFile << std::endl;
        }
        catch (std::exception const& e)
        {
            std::cerr << "cannot load state: " << e.what() << std::endl;
        }
    }
    a.audio.start();
    a.midi.start();
    Rest::registerRoutes<Api>(Rest::InstanceModel::ThreadSafeSingleton, 0);
    std::cout << "MuCoTo listening on " << listen << std::endl;
    cppgow::listenAndServe(listen);
    while (true)
        std::this_thread::sleep_for(std::chrono::seconds(1));
}
