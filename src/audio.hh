// MuCoTo audio engine: multithreaded audio processing DAG.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace audio
{
    using Buffer = std::vector<float>;

    /// Global sample storage, shared by all sound bank players.
    class SampleBank
    {
    public:
        void set(std::string const& name, std::shared_ptr<Buffer> data);
        std::shared_ptr<Buffer> get(std::string const& name) const;
        void remove(std::string const& name);
        std::vector<std::string> names() const;
        std::map<std::string, std::shared_ptr<Buffer>> all() const;
    private:
        mutable std::mutex _mutex;
        std::map<std::string, std::shared_ptr<Buffer>> _samples;
    };

    class Engine;

    /// Base class for all graph nodes. Parameters are thread safe.
    class Node
    {
    public:
        Node(std::string name): _name(std::move(name)) {}
        virtual ~Node() = default;
        virtual std::string type() const = 0;
        std::string const& name() const { return _name; }
        virtual int outputChannels() const { return 1; }
        /// inputs[i] is the buffer connected to input slot i, nullptr if absent.
        virtual void process(std::vector<Buffer const*> const& inputs, size_t frames) = 0;
        Buffer const& output(int channel) const { return _out[channel]; }

        void setParam(std::string const& key, float value);
        float param(std::string const& key, float def = 0.f) const;
        void setStringParam(std::string const& key, std::string const& value);
        std::string stringParam(std::string const& key, std::string const& def = "") const;
        std::map<std::string, float> params() const;
        std::map<std::string, std::string> stringParams() const;
    protected:
        virtual void onParam(std::string const&, float) {}
        virtual void onStringParam(std::string const&, std::string const&) {}
        std::string _name;
        mutable std::mutex _pmutex;
        std::map<std::string, float> _params;
        std::map<std::string, std::string> _sparams;
        std::vector<Buffer> _out = std::vector<Buffer>(1);
    };

    struct Link
    {
        std::string from;
        int fromChannel = 0;
        std::string to;
        int toInput = 0;
        bool operator==(Link const& o) const = default;
    };

    /// The audio processing DAG: nodes + links. Structure edits are locked
    /// against the engine which snapshots it at each cycle.
    class Graph
    {
    public:
        void addNode(std::shared_ptr<Node> node);
        void removeNode(std::string const& name);
        std::shared_ptr<Node> node(std::string const& name) const;
        std::vector<std::shared_ptr<Node>> nodes() const;
        /// Throws if nodes are missing or the link would create a cycle.
        void addLink(Link const& link);
        void removeLink(Link const& link);
        std::vector<Link> links() const;
        void clear();
        void snapshot(std::vector<std::shared_ptr<Node>>& nodes, std::vector<Link>& links) const;
    private:
        bool wouldCycle(Link const& link) const;
        mutable std::mutex _mutex;
        std::map<std::string, std::shared_ptr<Node>> _nodes;
        std::vector<Link> _links;
    };

    /// Pool of worker threads listening on a task channel.
    class ThreadPool
    {
    public:
        void start(int nThreads);
        void stop();
        void post(std::function<void()> task);
        ~ThreadPool() { stop(); }
    private:
        std::vector<std::thread> _threads;
        std::deque<std::function<void()>> _tasks;
        std::mutex _mutex;
        std::condition_variable _cv;
        bool _stop = false;
    };

    /// Owns the graph, the sample bank and the processing threads.
    /// The alsa input node clocks everything; without one (or if the device
    /// cannot be opened) a software clock paces the cycles so the graph
    /// still runs.
    class Engine
    {
    public:
        Engine();
        ~Engine();
        Graph& graph() { return _graph; }
        SampleBank& bank() { return _bank; }
        void start();
        void stop();
        bool running() const { return _running; }
        /// Sample rate of the whole graph (from the alsa input node).
        int rate() const;
        size_t frames() const;
        /// Create a node of the given registered type.
        std::shared_ptr<Node> createNode(std::string const& type, std::string const& name);
    private:
        void runLoop();
        void runCycle(std::vector<std::shared_ptr<Node>> const& nodes,
                      std::vector<Link> const& links, size_t frames);
        Graph _graph;
        SampleBank _bank;
        ThreadPool _pool;
        std::thread _clock;
        std::atomic<bool> _running = false;
    };

    // Concrete node types the rest of the app needs to talk to directly.

    class AlsaInputNode: public Node
    {
    public:
        AlsaInputNode(std::string name): Node(std::move(name)) {}
        ~AlsaInputNode();
        std::string type() const override { return "alsa_in"; }
        int outputChannels() const override;
        void process(std::vector<Buffer const*> const& inputs, size_t frames) override;
        /// True when the device is open and delivering data (and thus pacing us).
        bool isOpen() const { return _open; }
    protected:
        void onParam(std::string const&, float) override { _reopen = true; }
        void onStringParam(std::string const&, std::string const&) override { _reopen = true; }
    private:
        void ensureOpen();
        void closeDevice();
        void* _pcm = nullptr;
        std::vector<char> _raw;
        std::atomic<bool> _open = false;
        std::atomic<bool> _reopen = false;
    };

    class AlsaOutputNode: public Node
    {
    public:
        AlsaOutputNode(std::string name, Engine& engine): Node(std::move(name)), _engine(engine) {}
        ~AlsaOutputNode();
        std::string type() const override { return "alsa_out"; }
        int outputChannels() const override { return 0; }
        void process(std::vector<Buffer const*> const& inputs, size_t frames) override;
    protected:
        void onParam(std::string const&, float) override { _reopen = true; }
        void onStringParam(std::string const&, std::string const&) override { _reopen = true; }
    private:
        void ensureOpen(int channels);
        void closeDevice();
        Engine& _engine;
        void* _pcm = nullptr;
        int _channels = 0;
        std::vector<char> _raw;
        std::atomic<bool> _reopen = false;
    };

    class SoundBankPlayerNode: public Node
    {
    public:
        SoundBankPlayerNode(std::string name, SampleBank& bank): Node(std::move(name)), _bank(bank) {}
        std::string type() const override { return "player"; }
        void process(std::vector<Buffer const*> const& inputs, size_t frames) override;
        void play(std::string const& sample, bool loop);
        void stop(std::string const& sample = ""); ///< empty: stop everything
        bool playing(std::string const& sample = "") const;
    private:
        struct Voice
        {
            std::string sample;
            std::shared_ptr<Buffer> data;
            size_t pos = 0;
            bool loop = false;
        };
        SampleBank& _bank;
        mutable std::mutex _vmutex;
        std::vector<Voice> _voices;
    };

    class MixerNode: public Node
    {
    public:
        MixerNode(std::string name): Node(std::move(name)) {}
        std::string type() const override { return "mixer"; }
        void process(std::vector<Buffer const*> const& inputs, size_t frames) override;
    };

    class RecorderNode: public Node
    {
    public:
        RecorderNode(std::string name): Node(std::move(name))
        {
            setParam("duration", 30);
        }
        std::string type() const override { return "recorder"; }
        int outputChannels() const override { return 0; }
        void process(std::vector<Buffer const*> const& inputs, size_t frames) override;
        /// Move the recorded data out (clears the recorder).
        std::shared_ptr<Buffer> take();
        std::shared_ptr<Buffer> peek() const;
    private:
        int _rate = 48000;
        mutable std::mutex _dmutex;
        std::deque<float> _data;
    };

    class GainNode: public Node
    {
    public:
        GainNode(std::string name): Node(std::move(name)) { setParam("gain", 1); }
        std::string type() const override { return "gain"; }
        void process(std::vector<Buffer const*> const& inputs, size_t frames) override;
    };

    class QuantizerNode: public Node
    {
    public:
        QuantizerNode(std::string name): Node(std::move(name))
        {
            setParam("bits", 8);
            setParam("rate_divide", 1);
        }
        std::string type() const override { return "quantizer"; }
        void process(std::vector<Buffer const*> const& inputs, size_t frames) override;
    private:
        float _held = 0;
        int _holdCount = 0;
    };

    class DistortionNode: public Node
    {
    public:
        DistortionNode(std::string name): Node(std::move(name))
        {
            setParam("drive", 10);
            setParam("mix", 1);
        }
        std::string type() const override { return "distortion"; }
        void process(std::vector<Buffer const*> const& inputs, size_t frames) override;
    };

    /// Schroeder reverberator: parallel combs into serial allpasses.
    class ReverbNode: public Node
    {
    public:
        ReverbNode(std::string name, Engine& engine): Node(std::move(name)), _engine(engine)
        {
            setParam("mix", 0.3f);
            setParam("decay", 0.7f);
            setParam("damp", 0.4f);
        }
        std::string type() const override { return "reverb"; }
        void process(std::vector<Buffer const*> const& inputs, size_t frames) override;
    private:
        struct Comb
        {
            std::vector<float> buf;
            size_t pos = 0;
            float store = 0;
        };
        struct AllPass
        {
            std::vector<float> buf;
            size_t pos = 0;
        };
        void setup(int rate);
        Engine& _engine;
        int _rate = 0;
        std::vector<Comb> _combs;
        std::vector<AllPass> _allpasses;
    };
}
