#include "midi.hh"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>

#include <alsa/asoundlib.h>
#include <poll.h>

namespace midi
{
    namespace
    {
        // Launchpad-style palette entries, one per instrument / audio target.
        // +2 gives the darker variant of an entry.
        const int palette[] = {21, 45, 13, 37, 53, 9, 41, 17};
        const int paletteSize = sizeof(palette) / sizeof(palette[0]);
        const int colorWhite = 3;
        const int colorRed = 5;
        const double holdDelay = 1.0; ///< seconds before release-to-record
        const double doubleTap = 0.6; ///< double tap window, seconds

        std::string displayName(snd_seq_client_info_t* ci, snd_seq_port_info_t* pi)
        {
            return std::string(snd_seq_client_info_get_name(ci)) + ":"
                + snd_seq_port_info_get_name(pi);
        }
        const char* evName(int type)
        {
            switch (type)
            {
            case 0x80: return "note_off";
            case 0x90: return "note_on";
            case 0xA0: return "keypress";
            case 0xB0: return "cc";
            case 0xC0: return "pgmchange";
            case 0xD0: return "chanpress";
            case 0xE0: return "pitchbend";
            }
            return "?";
        }
    }

    MidiEngine::MidiEngine(audio::Engine& audio): _audio(audio)
    {
        snd_seq_t* seq = nullptr;
        if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0)
        {
            std::cerr << "cannot open alsa sequencer, midi disabled" << std::endl;
            return;
        }
        snd_seq_set_client_name(seq, "MuCoTo");
        _port = snd_seq_create_simple_port(seq, "io",
            SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_WRITE
            | SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_SUBS_WRITE,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        _seq = seq;
    }
    MidiEngine::~MidiEngine()
    {
        stop();
        if (_seq)
            snd_seq_close((snd_seq_t*)_seq);
    }
    double MidiEngine::now() const
    {
        static const auto t0 = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    void MidiEngine::start()
    {
        if (_running || !_seq)
            return;
        _running = true;
        _inputThread = std::thread([this] { inputLoop(); });
        _schedThread = std::thread([this] { schedulerLoop(); });
    }
    void MidiEngine::stop()
    {
        if (!_running)
            return;
        _running = false;
        _inputThread.join();
        _schedThread.join();
    }

    // ---------- port enumeration and connection ----------

    std::vector<PortInfo> MidiEngine::ports()
    {
        std::vector<PortInfo> res;
        if (!_seq)
            return res;
        auto seq = (snd_seq_t*)_seq;
        snd_seq_client_info_t* ci;
        snd_seq_port_info_t* pi;
        snd_seq_client_info_alloca(&ci);
        snd_seq_port_info_alloca(&pi);
        snd_seq_client_info_set_client(ci, -1);
        int self = snd_seq_client_id(seq);
        while (snd_seq_query_next_client(seq, ci) >= 0)
        {
            int client = snd_seq_client_info_get_client(ci);
            if (client == self || client == SND_SEQ_CLIENT_SYSTEM)
                continue;
            snd_seq_port_info_set_client(pi, client);
            snd_seq_port_info_set_port(pi, -1);
            while (snd_seq_query_next_port(seq, pi) >= 0)
            {
                unsigned caps = snd_seq_port_info_get_capability(pi);
                if (caps & SND_SEQ_PORT_CAP_NO_EXPORT)
                    continue;
                PortInfo info;
                info.client = client;
                info.port = snd_seq_port_info_get_port(pi);
                info.name = displayName(ci, pi);
                info.isInput = (caps & SND_SEQ_PORT_CAP_READ)
                    && (caps & SND_SEQ_PORT_CAP_SUBS_READ);
                info.isOutput = (caps & SND_SEQ_PORT_CAP_WRITE)
                    && (caps & SND_SEQ_PORT_CAP_SUBS_WRITE);
                if (info.isInput || info.isOutput)
                    res.push_back(info);
            }
        }
        return res;
    }
    bool MidiEngine::subscribeTo(int client, int port)
    {
        auto key = std::make_pair(client, port);
        auto& count = _toSubs[key];
        if (count == 0)
        {
            int r = snd_seq_connect_to((snd_seq_t*)_seq, _port, client, port);
            if (r < 0 && r != -EBUSY)
            {
                _toSubs.erase(key);
                return false;
            }
        }
        count++;
        return true;
    }
    void MidiEngine::unsubscribeTo(int client, int port)
    {
        auto it = _toSubs.find({client, port});
        if (it == _toSubs.end())
            return;
        if (--it->second <= 0)
        {
            snd_seq_disconnect_to((snd_seq_t*)_seq, _port, client, port);
            _toSubs.erase(it);
        }
    }
    void MidiEngine::connectInput(InputCfg& in)
    {
        auto all = ports();
        for (auto const& p: all)
        {
            if (!p.isInput || (p.name != in.port && p.name.find(in.port) != 0))
                continue;
            if (snd_seq_connect_from((snd_seq_t*)_seq, _port, p.client, p.port) < 0)
                continue;
            in.srcClient = p.client;
            in.srcPort = p.port;
            in.connected = true;
            if (_debug)
                std::cerr << "[midi] input '" << in.port << "' connected from "
                          << p.client << ":" << p.port << std::endl;
            // led/feedback side: a writable port on the same client
            in.ledClient = -1;
            for (auto const& o: all)
                if (o.isOutput && o.client == p.client
                    && (in.ledClient < 0 || o.port == p.port))
                {
                    in.ledClient = o.client;
                    in.ledPort = o.port;
                }
            // hardware ports only open their rawmidi device on subscription,
            // direct sends without one fail with ENODEV. Only loopers need the
            // write side (led feedback), keep plain inputs read-only.
            if (in.looper && in.ledClient >= 0)
            {
                if (!subscribeTo(in.ledClient, in.ledPort))
                    in.ledClient = -1;
                enterProgrammerMode(in);
                refreshAllLeds(in);
            }
            return;
        }
    }
    void MidiEngine::connectOutput(OutputCfg& out)
    {
        for (auto const& p: ports())
        {
            if (!p.isOutput || (p.name != out.port && p.name.find(out.port) != 0))
                continue;
            // subscribe so kernel hardware ports open their rawmidi device
            // (events are still addressed directly to the port)
            if (!subscribeTo(p.client, p.port))
                continue;
            out.dstClient = p.client;
            out.dstPort = p.port;
            out.connected = true;
            if (_debug)
                std::cerr << "[midi] output " << out.id << " '" << out.port
                          << "' -> " << p.client << ":" << p.port << std::endl;
            return;
        }
    }
    void MidiEngine::rescan()
    {
        std::lock_guard l(_mutex);
        for (auto& [id, in]: _inputs)
            if (!in.connected)
                connectInput(in);
        for (auto& [id, out]: _outputs)
            if (!out.connected)
                connectOutput(out);
    }

    // ---------- configuration ----------

    int MidiEngine::addInput(std::string const& portName)
    {
        std::lock_guard l(_mutex);
        InputCfg in;
        in.id = _nextId++;
        in.port = portName;
        connectInput(in);
        _inputs[in.id] = in;
        return in.id;
    }
    int MidiEngine::addOutput(std::string const& portName)
    {
        std::lock_guard l(_mutex);
        OutputCfg out;
        out.id = _nextId++;
        out.port = portName;
        connectOutput(out);
        _outputs[out.id] = out;
        return out.id;
    }
    void MidiEngine::removeInput(int id)
    {
        std::lock_guard l(_mutex);
        auto it = _inputs.find(id);
        if (it == _inputs.end())
            return;
        if (it->second.connected)
            snd_seq_disconnect_from((snd_seq_t*)_seq, _port,
                                    it->second.srcClient, it->second.srcPort);
        if (it->second.looper && it->second.ledClient >= 0)
            unsubscribeTo(it->second.ledClient, it->second.ledPort);
        _inputs.erase(it);
    }
    void MidiEngine::removeOutput(int id)
    {
        std::lock_guard l(_mutex);
        auto it = _outputs.find(id);
        if (it == _outputs.end())
            return;
        if (it->second.connected)
            unsubscribeTo(it->second.dstClient, it->second.dstPort);
        _outputs.erase(it);
    }
    void MidiEngine::setLooper(int id, bool looper)
    {
        std::lock_guard l(_mutex);
        auto it = _inputs.find(id);
        if (it == _inputs.end() || it->second.looper == looper)
            return;
        auto& in = it->second;
        in.looper = looper;
        if (looper)
        {
            if (in.connected && in.ledClient >= 0 && !subscribeTo(in.ledClient, in.ledPort))
                in.ledClient = -1;
            if (in.connected)
            {
                enterProgrammerMode(in);
                refreshAllLeds(in);
            }
        }
        else if (in.ledClient >= 0)
            unsubscribeTo(in.ledClient, in.ledPort);
    }
    void MidiEngine::setDefaultRoute(int inputId, int outputId)
    {
        std::lock_guard l(_mutex);
        auto it = _inputs.find(inputId);
        if (it != _inputs.end())
            it->second.defaultRoute = outputId;
    }
    void MidiEngine::setChannelRoute(int inputId, int channel, int outputId)
    {
        std::lock_guard l(_mutex);
        auto it = _inputs.find(inputId);
        if (it == _inputs.end())
            return;
        if (outputId < 0)
            it->second.channelRoutes.erase(channel);
        else
            it->second.channelRoutes[channel] = outputId;
    }

    // ---------- learn ----------

    void MidiEngine::learn(LearnTarget const& target)
    {
        std::lock_guard l(_mutex);
        _pending = target;
    }
    void MidiEngine::cancelLearn()
    {
        std::lock_guard l(_mutex);
        _pending.reset();
    }
    bool MidiEngine::learning() const
    {
        std::lock_guard l(_mutex);
        return _pending.has_value();
    }
    void MidiEngine::removeBinding(int index)
    {
        std::lock_guard l(_mutex);
        if (index >= 0 && index < (int)_bindings.size())
            _bindings.erase(_bindings.begin() + index);
    }

    // ---------- transport ----------

    void MidiEngine::setBpm(double bpm)
    {
        _bpm = std::clamp(bpm, 20., 400.);
    }
    double MidiEngine::bpm() const
    {
        return _bpm;
    }
    bool MidiEngine::rolling() const
    {
        std::lock_guard l(_mutex);
        return _rollStart > 0;
    }
    double MidiEngine::measures() const
    {
        std::lock_guard l(_mutex);
        if (_rollStart <= 0)
            return 0;
        return (now() - _rollStart) * _bpm / 240.;
    }

    // ---------- state access ----------

    std::map<int, InputCfg> MidiEngine::inputs() const
    {
        std::lock_guard l(_mutex);
        return _inputs;
    }
    std::map<int, OutputCfg> MidiEngine::outputs() const
    {
        std::lock_guard l(_mutex);
        return _outputs;
    }
    std::vector<Binding> MidiEngine::bindings() const
    {
        std::lock_guard l(_mutex);
        return _bindings;
    }
    std::pair<int, int> MidiEngine::currentPad() const
    {
        std::lock_guard l(_mutex);
        return _currentPad;
    }
    void MidiEngine::restore(std::map<int, InputCfg> const& inputs,
                             std::map<int, OutputCfg> const& outputs,
                             std::vector<Binding> const& bindings, double bpm)
    {
        std::lock_guard l(_mutex);
        for (auto& [id, in]: _inputs)
        {
            if (in.connected)
                snd_seq_disconnect_from((snd_seq_t*)_seq, _port, in.srcClient, in.srcPort);
            if (in.ledClient >= 0)
                snd_seq_disconnect_to((snd_seq_t*)_seq, _port, in.ledClient, in.ledPort);
        }
        for (auto& [id, out]: _outputs)
            if (out.connected)
                snd_seq_disconnect_to((snd_seq_t*)_seq, _port, out.dstClient, out.dstPort);
        _inputs = inputs;
        _outputs = outputs;
        _bindings = bindings;
        _bpm = bpm;
        _currentPad = {-1, -1};
        _rollStart = 0;
        for (auto& [id, in]: _inputs)
        {
            in.connected = false;
            in.srcClient = in.ledClient = -1;
            for (auto& [note, pad]: in.pads)
                if (pad.st != Pad::St::Empty)
                    pad.st = pad.target ? Pad::St::Idle : Pad::St::Empty;
            _nextId = std::max(_nextId, id + 1);
            connectInput(in);
        }
        for (auto& [id, out]: _outputs)
        {
            out.connected = false;
            _nextId = std::max(_nextId, id + 1);
            connectOutput(out);
        }
    }

    Clip MidiEngine::clipOf(int inputId, int note) const
    {
        std::lock_guard l(_mutex);
        auto it = _inputs.find(inputId);
        if (it == _inputs.end())
            return {};
        auto pit = it->second.pads.find(note);
        return pit == it->second.pads.end() ? Clip{} : pit->second.clip;
    }
    void MidiEngine::setClip(int inputId, int note, Clip const& clip)
    {
        std::lock_guard l(_mutex);
        auto it = _inputs.find(inputId);
        if (it == _inputs.end())
            return;
        auto pit = it->second.pads.find(note);
        if (pit == it->second.pads.end())
            return;
        pit->second.clip = clip;
        updateLed(it->second, note, pit->second);
    }
    void MidiEngine::clearPad(int inputId, int note)
    {
        std::lock_guard l(_mutex);
        auto it = _inputs.find(inputId);
        if (it == _inputs.end())
            return;
        auto pit = it->second.pads.find(note);
        if (pit == it->second.pads.end())
            return;
        if (pit->second.st == Pad::St::PlayingOnce || pit->second.st == Pad::St::Looping)
            stopPlay(it->second, note, pit->second);
        pit->second = Pad{};
        updateLed(it->second, note, pit->second);
    }

    // ---------- node lookup ----------

    audio::SoundBankPlayerNode* MidiEngine::playerNode(std::string const& name) const
    {
        return dynamic_cast<audio::SoundBankPlayerNode*>(_audio.graph().node(name).get());
    }
    audio::RecorderNode* MidiEngine::recorderNode(std::string const& name) const
    {
        return dynamic_cast<audio::RecorderNode*>(_audio.graph().node(name).get());
    }
    std::string MidiEngine::genSampleName()
    {
        while (true)
        {
            auto name = "rec_" + std::to_string(_sampleCounter++);
            if (!_audio.bank().get(name))
                return name;
        }
    }

    // ---------- midi io ----------

    void MidiEngine::sendToOutput(int outputId, int type, int channel, int d1, int d2)
    {
        auto it = _outputs.find(outputId);
        if (it == _outputs.end() || !it->second.connected)
        {
            if (_debug)
                std::cerr << "[midi]   send to out " << outputId << " dropped ("
                          << (it == _outputs.end() ? "unknown output" : "not connected")
                          << ")" << std::endl;
            return;
        }
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        switch (type)
        {
        case 0x90:
            snd_seq_ev_set_noteon(&ev, channel, d1, d2);
            break;
        case 0x80:
            snd_seq_ev_set_noteoff(&ev, channel, d1, d2);
            break;
        case 0xA0:
            snd_seq_ev_set_keypress(&ev, channel, d1, d2);
            break;
        case 0xB0:
            snd_seq_ev_set_controller(&ev, channel, d1, d2);
            break;
        case 0xC0:
            snd_seq_ev_set_pgmchange(&ev, channel, d1);
            break;
        case 0xD0:
            snd_seq_ev_set_chanpress(&ev, channel, d1);
            break;
        case 0xE0:
            snd_seq_ev_set_pitchbend(&ev, channel, (d1 | (d2 << 7)) - 8192);
            break;
        default:
            return;
        }
        snd_seq_ev_set_source(&ev, _port);
        snd_seq_ev_set_dest(&ev, it->second.dstClient, it->second.dstPort);
        snd_seq_ev_set_direct(&ev);
        int r = snd_seq_event_output_direct((snd_seq_t*)_seq, &ev);
        if (r < 0)
            std::cerr << "[midi] send " << evName(type) << " to out " << outputId
                      << " (" << it->second.dstClient << ":" << it->second.dstPort
                      << ") failed: " << snd_strerror(r) << std::endl;
    }
    void MidiEngine::allNotesOff(int outputId)
    {
        for (int c = 0; c < 16; c++)
            sendToOutput(outputId, 0xB0, c, 123, 0);
    }
    void MidiEngine::sendSysex(int client, int port, std::vector<unsigned char> const& payload)
    {
        std::vector<unsigned char> data;
        data.push_back(0xF0);
        // Launchpad MK3 header
        for (unsigned char b: {0x00, 0x20, 0x29, 0x02, 0x0D})
            data.push_back(b);
        data.insert(data.end(), payload.begin(), payload.end());
        data.push_back(0xF7);
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_sysex(&ev, data.size(), data.data());
        snd_seq_ev_set_source(&ev, _port);
        snd_seq_ev_set_dest(&ev, client, port);
        snd_seq_ev_set_direct(&ev);
        snd_seq_event_output_direct((snd_seq_t*)_seq, &ev);
    }
    void MidiEngine::enterProgrammerMode(InputCfg& in)
    {
        if (in.ledClient >= 0)
            sendSysex(in.ledClient, in.ledPort, {0x0E, 1});
    }
    int MidiEngine::padColor(Pad const& pad) const
    {
        if (pad.target == 1)
        {
            int idx = 0;
            for (auto const& [id, out]: _outputs)
            {
                if (id == pad.instrument)
                    break;
                idx++;
            }
            return palette[idx % paletteSize];
        }
        if (pad.target == 2)
        {
            size_t h = std::hash<std::string>()(pad.player.empty() ? pad.recorder : pad.player);
            return palette[h % paletteSize];
        }
        return 0;
    }
    void MidiEngine::updateLed(InputCfg& in, int note, Pad const& pad)
    {
        if (!in.looper || in.ledClient < 0)
            return;
        std::vector<unsigned char> spec{0x03};
        switch (pad.st)
        {
        case Pad::St::Empty:
            spec.insert(spec.end(), {0, (unsigned char)note, 0});
            break;
        case Pad::St::Selected:
            spec.insert(spec.end(), {0, (unsigned char)note, (unsigned char)colorWhite});
            break;
        case Pad::St::Recording:
            spec.insert(spec.end(), {2, (unsigned char)note, (unsigned char)colorRed});
            break;
        case Pad::St::Idle:
        {
            bool hasContent = pad.target == 1 ? !pad.clip.events.empty() : !pad.sample.empty();
            int color = padColor(pad) + (hasContent ? 0 : 2);
            spec.insert(spec.end(), {0, (unsigned char)note, (unsigned char)color});
            break;
        }
        case Pad::St::PlayingOnce:
        case Pad::St::Looping:
            spec.insert(spec.end(),
                        {1, (unsigned char)note, (unsigned char)padColor(pad), 0});
            break;
        }
        sendSysex(in.ledClient, in.ledPort, spec);
    }
    void MidiEngine::refreshAllLeds(InputCfg& in)
    {
        for (auto const& [note, pad]: in.pads)
            updateLed(in, note, pad);
    }

    // ---------- looper pad state machine ----------

    void MidiEngine::handlePadPress(InputCfg& in, int note)
    {
        auto& pad = in.pads[note];
        _currentPad = {in.id, note};
        pad.pressTime = now();
        if (pad.st == Pad::St::Recording)
        {
            stopRecording(in, note, pad);
            pad.pressTime = -1; // swallow the matching release
            return;
        }
        if (pad.st == Pad::St::Empty)
        {
            pad.st = Pad::St::Selected;
            updateLed(in, note, pad);
        }
    }
    void MidiEngine::handlePadRelease(InputCfg& in, int note)
    {
        auto it = in.pads.find(note);
        if (it == in.pads.end())
            return;
        auto& pad = it->second;
        if (pad.pressTime < 0)
            return;
        double t = now();
        double held = t - pad.pressTime;
        if (held >= holdDelay && pad.target != 0
            && (pad.st == Pad::St::Idle || pad.st == Pad::St::Selected))
        {
            startRecording(in, note, pad);
            return;
        }
        switch (pad.st)
        {
        case Pad::St::PlayingOnce:
            if (t - pad.lastTap < doubleTap)
            {
                // second hit: switch to looping
                pad.st = Pad::St::Looping;
                if (pad.target == 2)
                    if (auto p = playerNode(pad.player))
                    {
                        p->stop(pad.sample);
                        p->play(pad.sample, true);
                    }
                updateLed(in, note, pad);
            }
            else
                stopPlay(in, note, pad);
            break;
        case Pad::St::Looping:
            stopPlay(in, note, pad);
            break;
        case Pad::St::Idle:
            if ((pad.target == 1 && !pad.clip.events.empty())
                || (pad.target == 2 && !pad.sample.empty()))
                startPlay(in, note, pad, false);
            break;
        default:
            break;
        }
        pad.lastTap = t;
    }
    void MidiEngine::startRecording(InputCfg& in, int note, Pad& pad)
    {
        if (pad.target == 1)
        {
            pad.clip.events.clear();
            pad.clip.length = 0;
            pad.recordStart = now();
        }
        else if (pad.target == 2)
        {
            auto rec = recorderNode(pad.recorder);
            if (!rec)
                return;
            rec->setParam("clear", 1);
            rec->setParam("enabled", 1);
        }
        pad.st = Pad::St::Recording;
        updateLed(in, note, pad);
    }
    void MidiEngine::stopRecording(InputCfg& in, int note, Pad& pad)
    {
        if (pad.target == 1)
            pad.clip.length = now() - pad.recordStart;
        else if (pad.target == 2)
        {
            if (auto rec = recorderNode(pad.recorder))
            {
                rec->setParam("enabled", 0);
                auto data = rec->take();
                if (data && !data->empty())
                {
                    if (pad.sample.empty())
                        pad.sample = genSampleName();
                    _audio.bank().set(pad.sample, data);
                }
            }
        }
        pad.st = Pad::St::Idle;
        updateLed(in, note, pad);
    }
    void MidiEngine::startPlay(InputCfg& in, int note, Pad& pad, bool loop)
    {
        if (pad.target == 1)
        {
            pad.playStart = now();
            pad.evIndex = 0;
        }
        else if (pad.target == 2)
        {
            auto p = playerNode(pad.player);
            if (!p)
                return;
            p->play(pad.sample, loop);
        }
        pad.st = loop ? Pad::St::Looping : Pad::St::PlayingOnce;
        updateLed(in, note, pad);
    }
    void MidiEngine::stopPlay(InputCfg& in, int note, Pad& pad)
    {
        if (pad.target == 1)
            allNotesOff(pad.instrument);
        else if (pad.target == 2)
            if (auto p = playerNode(pad.player))
                p->stop(pad.sample);
        pad.st = Pad::St::Idle;
        updateLed(in, note, pad);
    }

    // ---------- cc / learn / bindings ----------

    void MidiEngine::handleCC(InputCfg& in, int channel, int cc, int value,
                              std::string const& dbg)
    {
        if (_pending)
        {
            if (value > 0)
            { // re-learning a bound cc replaces the previous binding
                std::erase_if(_bindings, [&](Binding const& b) {
                    return b.channel == channel && b.cc == cc;
                });
                _bindings.push_back(Binding{channel, cc, *_pending});
                if (_debug)
                    std::cerr << dbg << " -> learnt as '" << _pending->action << "'"
                              << std::endl;
                _pending.reset();
            }
            return;
        }
        for (auto const& b: _bindings)
            if (b.channel == channel && b.cc == cc)
            {
                if (_debug)
                    std::cerr << dbg << " -> binding " << b.target.action
                              << (b.target.node.empty() ? "" : " " + b.target.node)
                              << (b.target.param.empty() ? "" : "." + b.target.param)
                              << std::endl;
                executeBinding(b.target, value);
                return;
            }
        if (!in.looper)
        {
            int route = in.defaultRoute;
            auto rit = in.channelRoutes.find(channel);
            if (rit != in.channelRoutes.end())
                route = rit->second;
            if (route >= 0)
            {
                if (_debug)
                    std::cerr << dbg << " -> out " << route << std::endl;
                sendToOutput(route, 0xB0, channel, cc, value);
                recordRouted(route, 0xB0, channel, cc, value);
            }
            else if (_debug)
                std::cerr << dbg << " -> dropped (no route)" << std::endl;
        }
        else if (_debug)
            std::cerr << dbg << " -> dropped (looper, cc not bound)" << std::endl;
    }
    void MidiEngine::executeBinding(LearnTarget const& t, int value)
    {
        if (t.action == "node_param")
        {
            if (auto node = _audio.graph().node(t.node))
                node->setParam(t.param, t.min + (t.max - t.min) * value / 127.f);
        }
        else if (t.action == "default_route")
        {
            if (value > 0)
            {
                auto it = _inputs.find(t.input);
                if (it != _inputs.end())
                    it->second.defaultRoute = t.output;
            }
        }
        else if (t.action == "pad_instrument" || t.action == "pad_recorder"
                 || t.action == "pad_player")
        {
            if (value <= 0)
                return;
            auto it = _inputs.find(_currentPad.first);
            if (it == _inputs.end())
                return;
            auto pit = it->second.pads.find(_currentPad.second);
            if (pit == it->second.pads.end())
                return;
            auto& pad = pit->second;
            if (t.action == "pad_instrument")
            {
                pad.target = 1;
                pad.instrument = t.output;
            }
            else
            {
                pad.target = 2;
                if (t.action == "pad_recorder")
                    pad.recorder = t.node;
                else
                    pad.player = t.node;
            }
            if (pad.st == Pad::St::Selected || pad.st == Pad::St::Empty)
                pad.st = Pad::St::Idle;
            updateLed(it->second, _currentPad.second, pad);
        }
    }
    void MidiEngine::recordRouted(int outputId, int type, int channel, int d1, int d2)
    {
        for (auto& [id, in]: _inputs)
        {
            if (!in.looper)
                continue;
            for (auto& [note, pad]: in.pads)
                if (pad.st == Pad::St::Recording && pad.target == 1
                    && pad.instrument == outputId)
                    pad.clip.events.push_back(MidiEvent{
                        now() - pad.recordStart, type, channel, d1, d2});
        }
    }

    // ---------- event handling ----------

    void MidiEngine::handleEvent(void* seqEvent)
    {
        auto ev = (snd_seq_event_t*)seqEvent;
        std::lock_guard l(_mutex);
        InputCfg* in = nullptr;
        for (auto& [id, i]: _inputs)
            if (i.connected && i.srcClient == ev->source.client
                && i.srcPort == ev->source.port)
                in = &i;
        int type = 0, channel = 0, d1 = 0, d2 = 0;
        switch (ev->type)
        {
        case SND_SEQ_EVENT_NOTEON:
            type = ev->data.note.velocity ? 0x90 : 0x80;
            channel = ev->data.note.channel;
            d1 = ev->data.note.note;
            d2 = ev->data.note.velocity;
            break;
        case SND_SEQ_EVENT_NOTEOFF:
            type = 0x80;
            channel = ev->data.note.channel;
            d1 = ev->data.note.note;
            d2 = ev->data.note.velocity;
            break;
        case SND_SEQ_EVENT_KEYPRESS:
            type = 0xA0;
            channel = ev->data.note.channel;
            d1 = ev->data.note.note;
            d2 = ev->data.note.velocity;
            break;
        case SND_SEQ_EVENT_CONTROLLER:
            type = 0xB0;
            channel = ev->data.control.channel;
            d1 = ev->data.control.param;
            d2 = ev->data.control.value;
            break;
        case SND_SEQ_EVENT_PGMCHANGE:
            type = 0xC0;
            channel = ev->data.control.channel;
            d1 = ev->data.control.value;
            break;
        case SND_SEQ_EVENT_CHANPRESS:
            type = 0xD0;
            channel = ev->data.control.channel;
            d1 = ev->data.control.value;
            break;
        case SND_SEQ_EVENT_PITCHBEND:
        {
            type = 0xE0;
            channel = ev->data.control.channel;
            int v = ev->data.control.value + 8192;
            d1 = v & 0x7F;
            d2 = (v >> 7) & 0x7F;
            break;
        }
        default:
            if (_debug && ev->type != SND_SEQ_EVENT_CLOCK
                && ev->type != SND_SEQ_EVENT_SENSING)
                std::cerr << "[midi] " << (int)ev->source.client << ":"
                          << (int)ev->source.port << " seq event type "
                          << (int)ev->type << " ignored" << std::endl;
            return;
        }
        std::string dbg;
        if (_debug)
        {
            std::ostringstream ss;
            ss << "[midi] " << (int)ev->source.client << ":" << (int)ev->source.port
               << " " << (in ? "'" + in->port.substr(0, in->port.find(':')) + "'"
                             : "(unconfigured)")
               << " " << evName(type) << " ch" << channel << " " << d1 << " " << d2;
            dbg = ss.str();
        }
        if (!in)
        {
            if (_debug)
                std::cerr << dbg << " -> dropped (source matches no configured input)"
                          << std::endl;
            return;
        }
        if (type == 0xB0)
        {
            handleCC(*in, channel, d1, d2, dbg);
            return;
        }
        if (in->looper)
        {
            if (type == 0x90)
            {
                if (_debug)
                    std::cerr << dbg << " -> looper pad press" << std::endl;
                handlePadPress(*in, d1);
            }
            else if (type == 0x80)
            {
                if (_debug)
                    std::cerr << dbg << " -> looper pad release" << std::endl;
                handlePadRelease(*in, d1);
            }
            else if (_debug)
                std::cerr << dbg << " -> dropped (looper)" << std::endl;
            return;
        }
        int route = in->defaultRoute;
        auto rit = in->channelRoutes.find(channel);
        if (rit != in->channelRoutes.end())
            route = rit->second;
        if (route < 0)
        {
            if (_debug)
                std::cerr << dbg << " -> dropped (no route)" << std::endl;
            return;
        }
        if (_debug)
            std::cerr << dbg << " -> out " << route << std::endl;
        sendToOutput(route, type, channel, d1, d2);
        recordRouted(route, type, channel, d1, d2);
    }
    void MidiEngine::inputLoop()
    {
        auto seq = (snd_seq_t*)_seq;
        int nfds = snd_seq_poll_descriptors_count(seq, POLLIN);
        std::vector<pollfd> fds(nfds);
        snd_seq_poll_descriptors(seq, fds.data(), nfds, POLLIN);
        while (_running)
        {
            if (poll(fds.data(), nfds, 100) <= 0)
                continue;
            snd_seq_event_t* ev = nullptr;
            while (snd_seq_event_input_pending(seq, 1) > 0)
            {
                if (snd_seq_event_input(seq, &ev) < 0)
                    break;
                if (ev)
                    handleEvent(ev);
            }
        }
    }

    // ---------- clip scheduler ----------

    void MidiEngine::schedulerLoop()
    {
        while (_running)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            std::lock_guard l(_mutex);
            double t = now();
            bool anyPlaying = false;
            for (auto& [id, in]: _inputs)
            {
                if (!in.looper)
                    continue;
                for (auto& [note, pad]: in.pads)
                {
                    if (pad.st != Pad::St::PlayingOnce && pad.st != Pad::St::Looping)
                        continue;
                    anyPlaying = true;
                    if (pad.target == 1)
                    {
                        double pos = t - pad.playStart;
                        auto const& evs = pad.clip.events;
                        while (pad.evIndex < evs.size() && evs[pad.evIndex].t <= pos)
                        {
                            auto const& e = evs[pad.evIndex++];
                            sendToOutput(pad.instrument, e.type, e.channel, e.d1, e.d2);
                        }
                        if (pos >= pad.clip.length)
                        {
                            if (pad.st == Pad::St::Looping)
                            {
                                pad.playStart += pad.clip.length;
                                pad.evIndex = 0;
                            }
                            else
                            {
                                allNotesOff(pad.instrument);
                                pad.st = Pad::St::Idle;
                                updateLed(in, note, pad);
                            }
                        }
                    }
                    else if (pad.target == 2 && pad.st == Pad::St::PlayingOnce)
                    {
                        auto p = playerNode(pad.player);
                        if (!p || !p->playing(pad.sample))
                        {
                            pad.st = Pad::St::Idle;
                            updateLed(in, note, pad);
                        }
                    }
                }
            }
            if (anyPlaying && _rollStart <= 0)
                _rollStart = t;
            else if (!anyPlaying)
                _rollStart = 0;
            if (t - _lastRescan > 2)
            {
                _lastRescan = t;
                for (auto& [id, in]: _inputs)
                    if (!in.connected)
                        connectInput(in);
                for (auto& [id, out]: _outputs)
                    if (!out.connected)
                        connectOutput(out);
            }
        }
    }
}
