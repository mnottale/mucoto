// MuCoTo MIDI path: alsa seq I/O, routing, looper devices, MIDI learn.
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "audio.hh"

namespace midi
{
    struct PortInfo
    {
        int client = 0;
        int port = 0;
        std::string name; ///< "client:port" display name
        bool isInput = false; ///< we can read from it
        bool isOutput = false; ///< we can write to it
    };

    struct MidiEvent
    {
        double t = 0; ///< seconds from clip start
        int type = 0; ///< raw status high nibble (0x90 note on, 0x80 off, 0xB0 cc...)
        int channel = 0;
        int d1 = 0;
        int d2 = 0;
    };

    struct Clip
    {
        std::vector<MidiEvent> events;
        double length = 0;
    };

    /// One pad of a looper device.
    struct Pad
    {
        enum class St
        {
            Empty,      ///< nothing assigned, led off
            Selected,   ///< pressed once, waiting for an assignment, white
            Recording,  ///< red
            Idle,       ///< assigned, solid colour (dark when no clip yet)
            PlayingOnce,///< flashing
            Looping     ///< flashing
        };
        int target = 0; ///< 0 none, 1 midi instrument, 2 audio
        int instrument = -1; ///< midi output id
        std::string recorder; ///< audio recorder node name
        std::string player; ///< audio sound bank player node name
        std::string sample; ///< sample name in the bank
        Clip clip;
        // runtime
        St st = St::Empty;
        double pressTime = 0;
        double lastTap = 0;
        double recordStart = 0;
        double playStart = 0;
        size_t evIndex = 0;
    };

    struct InputCfg
    {
        int id = 0;
        std::string port; ///< configured port name
        bool looper = false;
        int defaultRoute = -1; ///< output id
        std::map<int, int> channelRoutes; ///< midi channel -> output id
        std::map<int, Pad> pads; ///< note -> pad (loopers only)
        // runtime
        int srcClient = -1, srcPort = -1;
        int ledClient = -1, ledPort = -1; ///< write side of the pad device
        bool connected = false;
        int tempoLed = -1; ///< painted tempo row state (-1 force, -2 idle, 0-7 beat)
    };

    struct OutputCfg
    {
        int id = 0;
        std::string port;
        // runtime
        int dstClient = -1, dstPort = -1;
        bool connected = false;
    };

    struct LearnTarget
    {
        std::string action; ///< node_param, default_route, pad_instrument, pad_recorder, pad_player, program
        std::string node; ///< node_param / pad_recorder / pad_player
        std::string param; ///< node_param
        int input = -1; ///< default_route
        int output = -1; ///< default_route / pad_instrument / program
        float min = 0, max = 1; ///< node_param / program value scaling
        int outChannel = 0; ///< program: midi channel the message is sent on
    };

    struct Binding
    {
        int channel = 0;
        int cc = 0;
        LearnTarget target;
    };

    class MidiEngine
    {
    public:
        MidiEngine(audio::Engine& audio);
        ~MidiEngine();
        void start();
        void stop();
        /// Dump incoming midi events and routing decisions to stderr.
        void setDebug(bool debug) { _debug = debug; }

        std::vector<PortInfo> ports();

        // configuration (all thread safe)
        int addInput(std::string const& portName);
        int addOutput(std::string const& portName);
        void removeInput(int id);
        void removeOutput(int id);
        void setLooper(int id, bool looper);
        void setDefaultRoute(int inputId, int outputId);
        void setChannelRoute(int inputId, int channel, int outputId);
        void programChange(int outputId, int channel, int program);
        void rescan(); ///< try to (re)connect configured ports

        // learn
        void learn(LearnTarget const& target);
        void cancelLearn();
        bool learning() const;
        void removeBinding(int index);

        // transport
        void setBpm(double bpm);
        double bpm() const;
        bool rolling() const;
        double measures() const; ///< current measure position (4/4), 0 when idle
        long beats() const; ///< metronome beat count, free-running when idle

        // state access (used by the REST layer and save/load)
        std::map<int, InputCfg> inputs() const;
        std::map<int, OutputCfg> outputs() const;
        std::vector<Binding> bindings() const;
        void restore(std::map<int, InputCfg> const& inputs,
                     std::map<int, OutputCfg> const& outputs,
                     std::vector<Binding> const& bindings, double bpm);
        std::pair<int, int> currentPad() const; ///< inputId, note (-1,-1 when none)
        Clip clipOf(int inputId, int note) const;
        void setClip(int inputId, int note, Clip const& clip);
        void clearPad(int inputId, int note);

    private:
        double now() const;
        long beatsAt(double t) const; ///< callers hold _mutex
        void inputLoop();
        void schedulerLoop();
        void handleEvent(void* seqEvent);
        void handlePadPress(InputCfg& in, int note);
        void handlePadRelease(InputCfg& in, int note);
        void handleCC(InputCfg& in, int channel, int cc, int value,
                      std::string const& dbg);
        void executeBinding(LearnTarget const& t, int value);
        void startRecording(InputCfg& in, int note, Pad& pad);
        void stopRecording(InputCfg& in, int note, Pad& pad);
        void startPlay(InputCfg& in, int note, Pad& pad, bool loop);
        void stopPlay(InputCfg& in, int note, Pad& pad);
        void recordRouted(int outputId, int type, int channel, int d1, int d2);
        void sendToOutput(int outputId, int type, int channel, int d1, int d2);
        void allNotesOff(int outputId);
        void updateLed(InputCfg& in, int note, Pad const& pad);
        void paintTempoRow(InputCfg& in, int led, bool downbeat);
        void enterProgrammerMode(InputCfg& in);
        void refreshAllLeds(InputCfg& in);
        void sendSysex(int client, int port, std::vector<unsigned char> const& payload);
        /// Refcounted write-side subscriptions: a port can be used both as an
        /// instrument output and as a looper led target.
        bool subscribeTo(int client, int port);
        void unsubscribeTo(int client, int port);
        int padColor(Pad const& pad) const;
        void connectInput(InputCfg& in);
        void connectOutput(OutputCfg& out);
        std::string genSampleName();
        audio::SoundBankPlayerNode* playerNode(std::string const& name) const;
        audio::RecorderNode* recorderNode(std::string const& name) const;

        audio::Engine& _audio;
        void* _seq = nullptr; ///< snd_seq_t
        int _port = -1; ///< our own seq port
        std::atomic<bool> _running = false;
        std::atomic<bool> _debug = false;
        std::thread _inputThread;
        std::thread _schedThread;
        mutable std::mutex _mutex;
        std::map<int, InputCfg> _inputs;
        std::map<int, OutputCfg> _outputs;
        std::vector<Binding> _bindings;
        std::map<std::pair<int, int>, int> _toSubs; ///< (client,port) -> refcount
        std::optional<LearnTarget> _pending;
        int _nextId = 1;
        int _sampleCounter = 1;
        std::pair<int, int> _currentPad = {-1, -1};
        std::atomic<double> _bpm = 120;
        double _rollStart = 0; ///< now() when rolling started, 0 = not rolling
        double _lastRescan = 0;
    };
}
