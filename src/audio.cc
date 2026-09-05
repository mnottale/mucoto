#include "audio.hh"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>

#include <alsa/asoundlib.h>

namespace audio
{
    // ---------- SampleBank ----------

    void SampleBank::set(std::string const& name, std::shared_ptr<Buffer> data)
    {
        std::lock_guard l(_mutex);
        _samples[name] = std::move(data);
    }
    std::shared_ptr<Buffer> SampleBank::get(std::string const& name) const
    {
        std::lock_guard l(_mutex);
        auto it = _samples.find(name);
        return it == _samples.end() ? nullptr : it->second;
    }
    void SampleBank::remove(std::string const& name)
    {
        std::lock_guard l(_mutex);
        _samples.erase(name);
    }
    std::vector<std::string> SampleBank::names() const
    {
        std::lock_guard l(_mutex);
        std::vector<std::string> res;
        for (auto const& [k, v]: _samples)
            res.push_back(k);
        return res;
    }
    std::map<std::string, std::shared_ptr<Buffer>> SampleBank::all() const
    {
        std::lock_guard l(_mutex);
        return _samples;
    }

    // ---------- Node ----------

    void Node::setParam(std::string const& key, float value)
    {
        {
            std::lock_guard l(_pmutex);
            _params[key] = value;
        }
        onParam(key, value);
    }
    float Node::param(std::string const& key, float def) const
    {
        std::lock_guard l(_pmutex);
        auto it = _params.find(key);
        return it == _params.end() ? def : it->second;
    }
    void Node::setStringParam(std::string const& key, std::string const& value)
    {
        {
            std::lock_guard l(_pmutex);
            _sparams[key] = value;
        }
        onStringParam(key, value);
    }
    std::string Node::stringParam(std::string const& key, std::string const& def) const
    {
        std::lock_guard l(_pmutex);
        auto it = _sparams.find(key);
        return it == _sparams.end() ? def : it->second;
    }
    std::map<std::string, float> Node::params() const
    {
        std::lock_guard l(_pmutex);
        return _params;
    }
    std::map<std::string, std::string> Node::stringParams() const
    {
        std::lock_guard l(_pmutex);
        return _sparams;
    }

    // ---------- Graph ----------

    void Graph::addNode(std::shared_ptr<Node> node)
    {
        std::lock_guard l(_mutex);
        if (_nodes.count(node->name()))
            throw std::runtime_error("node name already in use: " + node->name());
        _nodes[node->name()] = std::move(node);
    }
    void Graph::removeNode(std::string const& name)
    {
        std::lock_guard l(_mutex);
        _nodes.erase(name);
        std::erase_if(_links, [&](Link const& l) { return l.from == name || l.to == name; });
    }
    std::shared_ptr<Node> Graph::node(std::string const& name) const
    {
        std::lock_guard l(_mutex);
        auto it = _nodes.find(name);
        return it == _nodes.end() ? nullptr : it->second;
    }
    std::vector<std::shared_ptr<Node>> Graph::nodes() const
    {
        std::lock_guard l(_mutex);
        std::vector<std::shared_ptr<Node>> res;
        for (auto const& [k, v]: _nodes)
            res.push_back(v);
        return res;
    }
    bool Graph::wouldCycle(Link const& link) const
    { // reachable(to -> from)? then from->to closes a cycle
        std::set<std::string> seen;
        std::vector<std::string> stack{link.to};
        while (!stack.empty())
        {
            auto cur = stack.back();
            stack.pop_back();
            if (cur == link.from)
                return true;
            if (!seen.insert(cur).second)
                continue;
            for (auto const& l: _links)
                if (l.from == cur)
                    stack.push_back(l.to);
        }
        return false;
    }
    void Graph::addLink(Link const& link)
    {
        std::lock_guard l(_mutex);
        auto from = _nodes.find(link.from);
        auto to = _nodes.find(link.to);
        if (from == _nodes.end() || to == _nodes.end())
            throw std::runtime_error("link endpoint not found");
        if (link.fromChannel < 0 || link.fromChannel >= from->second->outputChannels())
            throw std::runtime_error("bad output channel");
        if (std::find(_links.begin(), _links.end(), link) != _links.end())
            return;
        for (auto const& other: _links)
            if (other.to == link.to && other.toInput == link.toInput)
                throw std::runtime_error("input slot already connected");
        if (wouldCycle(link))
            throw std::runtime_error("link would create a cycle");
        _links.push_back(link);
    }
    void Graph::removeLink(Link const& link)
    {
        std::lock_guard l(_mutex);
        std::erase(_links, link);
    }
    std::vector<Link> Graph::links() const
    {
        std::lock_guard l(_mutex);
        return _links;
    }
    void Graph::clear()
    {
        std::lock_guard l(_mutex);
        _nodes.clear();
        _links.clear();
    }
    void Graph::snapshot(std::vector<std::shared_ptr<Node>>& nodes, std::vector<Link>& links) const
    {
        std::lock_guard l(_mutex);
        nodes.clear();
        links.clear();
        for (auto const& [k, v]: _nodes)
            nodes.push_back(v);
        links = _links;
    }

    // ---------- ThreadPool ----------

    void ThreadPool::start(int nThreads)
    {
        _stop = false;
        for (int i = 0; i < nThreads; i++)
            _threads.emplace_back([this] {
                while (true)
                {
                    std::function<void()> task;
                    {
                        std::unique_lock l(_mutex);
                        _cv.wait(l, [this] { return _stop || !_tasks.empty(); });
                        if (_stop)
                            return;
                        task = std::move(_tasks.front());
                        _tasks.pop_front();
                    }
                    try
                    {
                        task();
                    }
                    catch (std::exception const& e)
                    {
                        std::cerr << "audio task error: " << e.what() << std::endl;
                    }
                }
            });
    }
    void ThreadPool::stop()
    {
        {
            std::lock_guard l(_mutex);
            _stop = true;
        }
        _cv.notify_all();
        for (auto& t: _threads)
            t.join();
        _threads.clear();
    }
    void ThreadPool::post(std::function<void()> task)
    {
        {
            std::lock_guard l(_mutex);
            _tasks.push_back(std::move(task));
        }
        _cv.notify_one();
    }

    // ---------- Engine ----------

    Engine::Engine()
    {
        int n = std::max(2u, std::thread::hardware_concurrency()) - 1;
        _pool.start(n);
    }
    Engine::~Engine()
    {
        stop();
        _pool.stop();
    }
    void Engine::start()
    {
        if (_running)
            return;
        _running = true;
        _clock = std::thread([this] { runLoop(); });
    }
    void Engine::stop()
    {
        if (!_running)
            return;
        _running = false;
        _clock.join();
    }
    int Engine::rate() const
    {
        for (auto const& n: _graph.nodes())
            if (n->type() == "alsa_in")
                return (int)n->param("rate", 48000);
        return 48000;
    }
    size_t Engine::frames() const
    {
        for (auto const& n: _graph.nodes())
            if (n->type() == "alsa_in")
                return (size_t)n->param("buffer_size", 512);
        return 512;
    }
    void Engine::runLoop()
    {
        auto next = std::chrono::steady_clock::now();
        while (_running)
        {
            std::vector<std::shared_ptr<Node>> nodes;
            std::vector<Link> links;
            _graph.snapshot(nodes, links);
            size_t nFrames = frames();
            int sampleRate = rate();

            // The alsa input clocks everything: process it inline (blocking
            // read). Fall back to a software clock when absent or not open.
            AlsaInputNode* alsaIn = nullptr;
            for (auto const& n: nodes)
                if (auto a = dynamic_cast<AlsaInputNode*>(n.get()))
                    alsaIn = a;
            if (alsaIn)
                alsaIn->process({}, nFrames);
            if (!alsaIn || !alsaIn->isOpen())
            {
                next += std::chrono::microseconds(1000000ll * nFrames / sampleRate);
                std::this_thread::sleep_until(next);
            }
            else
                next = std::chrono::steady_clock::now();
            if (!nodes.empty())
                runCycle(nodes, links, nFrames);
        }
    }
    void Engine::runCycle(std::vector<std::shared_ptr<Node>> const& nodes,
                          std::vector<Link> const& links, size_t frames)
    {
        struct NodeState
        {
            std::shared_ptr<Node> node;
            std::vector<Buffer const*> inputs;
            std::atomic<int> pending = 0;
            std::vector<int> dependents; // indices into states
        };
        std::vector<NodeState> states(nodes.size());
        std::map<std::string, int> index;
        for (size_t i = 0; i < nodes.size(); i++)
        {
            states[i].node = nodes[i];
            index[nodes[i]->name()] = i;
        }
        for (auto const& l: links)
        {
            auto from = index.find(l.from);
            auto to = index.find(l.to);
            if (from == index.end() || to == index.end())
                continue;
            auto& src = states[from->second];
            auto& dst = states[to->second];
            if ((int)dst.inputs.size() <= l.toInput)
                dst.inputs.resize(l.toInput + 1, nullptr);
            dst.inputs[l.toInput] = &src.node->output(l.fromChannel);
            dst.pending++;
            src.dependents.push_back(to->second);
        }
        std::mutex mutex;
        std::condition_variable cv;
        size_t done = 0;
        std::function<void(int)> runNode = [&](int i) {
            auto& st = states[i];
            // alsa input was already processed inline by the clock thread
            if (!dynamic_cast<AlsaInputNode*>(st.node.get()))
            {
                try
                {
                    st.node->process(st.inputs, frames);
                }
                catch (std::exception const& e)
                {
                    std::cerr << "node " << st.node->name() << ": " << e.what() << std::endl;
                }
            }
            std::vector<int> ready;
            for (int d: st.dependents)
                if (--states[d].pending == 0)
                    ready.push_back(d);
            for (int d: ready)
                _pool.post([&, d] { runNode(d); });
            {
                std::lock_guard l(mutex);
                done++;
                cv.notify_one();
            }
        };
        for (size_t i = 0; i < states.size(); i++)
            if (states[i].pending == 0)
                _pool.post([&, i] { runNode((int)i); });
        std::unique_lock l(mutex);
        cv.wait(l, [&] { return done == states.size(); });
    }
    std::shared_ptr<Node> Engine::createNode(std::string const& type, std::string const& name)
    {
        if (type == "alsa_in")
        {
            for (auto const& n: _graph.nodes())
                if (n->type() == "alsa_in")
                    throw std::runtime_error("alsa_in is a singleton");
            return std::make_shared<AlsaInputNode>(name);
        }
        if (type == "alsa_out")
            return std::make_shared<AlsaOutputNode>(name, *this);
        if (type == "player")
            return std::make_shared<SoundBankPlayerNode>(name, _bank);
        if (type == "mixer")
            return std::make_shared<MixerNode>(name);
        if (type == "recorder")
            return std::make_shared<RecorderNode>(name);
        if (type == "gain")
            return std::make_shared<GainNode>(name);
        if (type == "quantizer")
            return std::make_shared<QuantizerNode>(name);
        if (type == "distortion")
            return std::make_shared<DistortionNode>(name);
        if (type == "reverb")
            return std::make_shared<ReverbNode>(name, *this);
        throw std::runtime_error("unknown node type: " + type);
    }

    // ---------- alsa helpers ----------

    namespace
    {
        snd_pcm_format_t parseFormat(std::string const& f)
        {
            if (f == "S32_LE")
                return SND_PCM_FORMAT_S32_LE;
            return SND_PCM_FORMAT_S16_LE;
        }
        int formatBytes(snd_pcm_format_t f)
        {
            return f == SND_PCM_FORMAT_S32_LE ? 4 : 2;
        }
        snd_pcm_t* openPcm(std::string const& device, snd_pcm_stream_t dir,
                           snd_pcm_format_t format, unsigned rate, unsigned channels,
                           snd_pcm_uframes_t period)
        {
            snd_pcm_t* pcm = nullptr;
            if (snd_pcm_open(&pcm, device.c_str(), dir, 0) < 0)
                return nullptr;
            snd_pcm_hw_params_t* hw;
            snd_pcm_hw_params_alloca(&hw);
            snd_pcm_hw_params_any(pcm, hw);
            snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
            snd_pcm_hw_params_set_format(pcm, hw, format);
            snd_pcm_hw_params_set_channels(pcm, hw, channels);
            snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, nullptr);
            snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, nullptr);
            snd_pcm_uframes_t bufSize = period * 4;
            snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &bufSize);
            if (snd_pcm_hw_params(pcm, hw) < 0)
            {
                snd_pcm_close(pcm);
                return nullptr;
            }
            snd_pcm_prepare(pcm);
            return pcm;
        }
    }

    // ---------- AlsaInputNode ----------

    AlsaInputNode::~AlsaInputNode()
    {
        closeDevice();
    }
    int AlsaInputNode::outputChannels() const
    {
        return std::max(1, (int)param("channels", 2));
    }
    void AlsaInputNode::closeDevice()
    {
        if (_pcm)
        {
            snd_pcm_close((snd_pcm_t*)_pcm);
            _pcm = nullptr;
        }
        _open = false;
    }
    void AlsaInputNode::ensureOpen()
    {
        if (_reopen.exchange(false))
            closeDevice();
        if (_pcm)
            return;
        auto format = parseFormat(stringParam("format", "S16_LE"));
        _pcm = openPcm(stringParam("device", "default"), SND_PCM_STREAM_CAPTURE, format,
                       (unsigned)param("rate", 48000), (unsigned)outputChannels(),
                       (snd_pcm_uframes_t)param("buffer_size", 512));
        _open = _pcm != nullptr;
    }
    void AlsaInputNode::process(std::vector<Buffer const*> const&, size_t frames)
    {
        int channels = outputChannels();
        _out.resize(channels);
        for (auto& b: _out)
            b.assign(frames, 0.f);
        ensureOpen();
        if (!_pcm)
            return;
        auto format = parseFormat(stringParam("format", "S16_LE"));
        int bytes = formatBytes(format);
        _raw.resize(frames * channels * bytes);
        auto r = snd_pcm_readi((snd_pcm_t*)_pcm, _raw.data(), frames);
        if (r < 0)
        {
            r = snd_pcm_recover((snd_pcm_t*)_pcm, r, 1);
            if (r < 0)
            {
                closeDevice();
                return;
            }
            r = snd_pcm_readi((snd_pcm_t*)_pcm, _raw.data(), frames);
            if (r < 0)
            {
                closeDevice();
                return;
            }
        }
        for (long f = 0; f < r; f++)
            for (int c = 0; c < channels; c++)
            {
                if (bytes == 2)
                {
                    int16_t v;
                    memcpy(&v, _raw.data() + (f * channels + c) * 2, 2);
                    _out[c][f] = v / 32768.f;
                }
                else
                {
                    int32_t v;
                    memcpy(&v, _raw.data() + (f * channels + c) * 4, 4);
                    _out[c][f] = v / 2147483648.f;
                }
            }
    }

    // ---------- AlsaOutputNode ----------

    AlsaOutputNode::~AlsaOutputNode()
    {
        closeDevice();
    }
    void AlsaOutputNode::closeDevice()
    {
        if (_pcm)
        {
            snd_pcm_close((snd_pcm_t*)_pcm);
            _pcm = nullptr;
        }
    }
    void AlsaOutputNode::ensureOpen(int channels)
    {
        if (_reopen.exchange(false) || channels != _channels)
            closeDevice();
        if (_pcm)
            return;
        _channels = channels;
        auto format = parseFormat(stringParam("format", "S16_LE"));
        _pcm = openPcm(stringParam("device", "default"), SND_PCM_STREAM_PLAYBACK, format,
                       (unsigned)_engine.rate(), (unsigned)channels,
                       (snd_pcm_uframes_t)_engine.frames());
    }
    void AlsaOutputNode::process(std::vector<Buffer const*> const& inputs, size_t frames)
    {
        int channels = 0;
        for (auto i: inputs)
            if (i)
                channels++;
        if (!channels)
            return;
        channels = std::min(channels, 2);
        ensureOpen(channels);
        if (!_pcm)
            return;
        auto format = parseFormat(stringParam("format", "S16_LE"));
        int bytes = formatBytes(format);
        _raw.assign(frames * channels * bytes, 0);
        int c = 0;
        for (auto in: inputs)
        {
            if (!in || c >= channels)
                continue;
            for (size_t f = 0; f < std::min(frames, in->size()); f++)
            {
                float v = std::clamp((*in)[f], -1.f, 1.f);
                if (bytes == 2)
                {
                    int16_t s = (int16_t)(v * 32767.f);
                    memcpy(_raw.data() + (f * channels + c) * 2, &s, 2);
                }
                else
                {
                    int32_t s = (int32_t)(v * 2147483647.f);
                    memcpy(_raw.data() + (f * channels + c) * 4, &s, 4);
                }
            }
            c++;
        }
        auto r = snd_pcm_writei((snd_pcm_t*)_pcm, _raw.data(), frames);
        if (r < 0)
        {
            r = snd_pcm_recover((snd_pcm_t*)_pcm, r, 1);
            if (r >= 0)
                snd_pcm_writei((snd_pcm_t*)_pcm, _raw.data(), frames);
        }
    }

    // ---------- SoundBankPlayerNode ----------

    void SoundBankPlayerNode::play(std::string const& sample, bool loop)
    {
        auto data = _bank.get(sample);
        if (!data || data->empty())
            return;
        std::lock_guard l(_vmutex);
        _voices.push_back(Voice{sample, data, 0, loop});
    }
    void SoundBankPlayerNode::stop(std::string const& sample)
    {
        std::lock_guard l(_vmutex);
        if (sample.empty())
            _voices.clear();
        else
            std::erase_if(_voices, [&](Voice const& v) { return v.sample == sample; });
    }
    bool SoundBankPlayerNode::playing(std::string const& sample) const
    {
        std::lock_guard l(_vmutex);
        if (sample.empty())
            return !_voices.empty();
        return std::any_of(_voices.begin(), _voices.end(),
                           [&](Voice const& v) { return v.sample == sample; });
    }
    void SoundBankPlayerNode::process(std::vector<Buffer const*> const&, size_t frames)
    {
        _out.resize(1);
        auto& out = _out[0];
        out.assign(frames, 0.f);
        float gain = param("gain", 1);
        std::lock_guard l(_vmutex);
        for (auto& v: _voices)
            for (size_t f = 0; f < frames; f++)
            {
                if (v.pos >= v.data->size())
                {
                    if (!v.loop)
                        break;
                    v.pos = 0;
                }
                out[f] += (*v.data)[v.pos++] * gain;
            }
        std::erase_if(_voices, [](Voice const& v) { return !v.loop && v.pos >= v.data->size(); });
    }

    // ---------- MixerNode ----------

    void MixerNode::process(std::vector<Buffer const*> const& inputs, size_t frames)
    {
        _out.resize(1);
        auto& out = _out[0];
        out.assign(frames, 0.f);
        for (size_t i = 0; i < inputs.size(); i++)
        {
            if (!inputs[i])
                continue;
            float gain = param("gain_" + std::to_string(i), 1);
            auto const& in = *inputs[i];
            for (size_t f = 0; f < std::min(frames, in.size()); f++)
                out[f] += in[f] * gain;
        }
    }

    // ---------- RecorderNode ----------

    void RecorderNode::process(std::vector<Buffer const*> const& inputs, size_t frames)
    {
        if (param("clear", 0) > 0.5f)
        {
            std::lock_guard l(_dmutex);
            _data.clear();
            setParam("clear", 0);
        }
        if (param("enabled", 0) < 0.5f || inputs.empty() || !inputs[0])
            return;
        auto const& in = *inputs[0];
        size_t maxSize = (size_t)(param("duration", 30) * _rate);
        std::lock_guard l(_dmutex);
        _data.insert(_data.end(), in.begin(), in.end());
        while (_data.size() > maxSize)
            _data.pop_front();
    }
    std::shared_ptr<Buffer> RecorderNode::take()
    {
        std::lock_guard l(_dmutex);
        auto res = std::make_shared<Buffer>(_data.begin(), _data.end());
        _data.clear();
        return res;
    }
    std::shared_ptr<Buffer> RecorderNode::peek() const
    {
        std::lock_guard l(_dmutex);
        return std::make_shared<Buffer>(_data.begin(), _data.end());
    }

    // ---------- Effects ----------

    void GainNode::process(std::vector<Buffer const*> const& inputs, size_t frames)
    {
        _out.resize(1);
        auto& out = _out[0];
        out.assign(frames, 0.f);
        if (inputs.empty() || !inputs[0])
            return;
        float g = param("gain", 1);
        auto const& in = *inputs[0];
        for (size_t f = 0; f < std::min(frames, in.size()); f++)
            out[f] = in[f] * g;
    }

    void QuantizerNode::process(std::vector<Buffer const*> const& inputs, size_t frames)
    {
        _out.resize(1);
        auto& out = _out[0];
        out.assign(frames, 0.f);
        if (inputs.empty() || !inputs[0])
            return;
        float bits = std::clamp(param("bits", 8), 1.f, 16.f);
        int divide = std::max(1, (int)param("rate_divide", 1));
        float levels = std::pow(2.f, bits - 1);
        auto const& in = *inputs[0];
        for (size_t f = 0; f < std::min(frames, in.size()); f++)
        {
            if (_holdCount <= 0)
            {
                _held = std::round(in[f] * levels) / levels;
                _holdCount = divide;
            }
            _holdCount--;
            out[f] = _held;
        }
    }

    void DistortionNode::process(std::vector<Buffer const*> const& inputs, size_t frames)
    {
        _out.resize(1);
        auto& out = _out[0];
        out.assign(frames, 0.f);
        if (inputs.empty() || !inputs[0])
            return;
        float drive = std::max(1.f, param("drive", 10));
        float mix = std::clamp(param("mix", 1), 0.f, 1.f);
        float norm = std::tanh(drive);
        auto const& in = *inputs[0];
        for (size_t f = 0; f < std::min(frames, in.size()); f++)
            out[f] = mix * std::tanh(in[f] * drive) / norm + (1 - mix) * in[f];
    }

    void ReverbNode::setup(int rate)
    {
        if (_rate == rate)
            return;
        _rate = rate;
        // classic freeverb-ish tunings, scaled from 44100
        static const int combT[] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int apT[] = {556, 441, 341, 225};
        _combs.clear();
        for (int t: combT)
        {
            Comb c;
            c.buf.assign(std::max(1, t * rate / 44100), 0.f);
            _combs.push_back(std::move(c));
        }
        _allpasses.clear();
        for (int t: apT)
        {
            AllPass a;
            a.buf.assign(std::max(1, t * rate / 44100), 0.f);
            _allpasses.push_back(std::move(a));
        }
    }
    void ReverbNode::process(std::vector<Buffer const*> const& inputs, size_t frames)
    {
        _out.resize(1);
        auto& out = _out[0];
        out.assign(frames, 0.f);
        if (inputs.empty() || !inputs[0])
            return;
        setup(_engine.rate());
        float mix = std::clamp(param("mix", 0.3f), 0.f, 1.f);
        float decay = std::clamp(param("decay", 0.7f), 0.f, 0.98f);
        float damp = std::clamp(param("damp", 0.4f), 0.f, 1.f);
        auto const& in = *inputs[0];
        for (size_t f = 0; f < std::min(frames, in.size()); f++)
        {
            float dry = in[f];
            float wet = 0;
            for (auto& c: _combs)
            {
                float y = c.buf[c.pos];
                c.store = y * (1 - damp) + c.store * damp;
                c.buf[c.pos] = dry + c.store * decay;
                c.pos = (c.pos + 1) % c.buf.size();
                wet += y;
            }
            wet /= _combs.size();
            for (auto& a: _allpasses)
            {
                float y = a.buf[a.pos];
                a.buf[a.pos] = wet + y * 0.5f;
                wet = y - wet * 0.5f;
                a.pos = (a.pos + 1) % a.buf.size();
            }
            out[f] = dry * (1 - mix) + wet * mix;
        }
    }
}
