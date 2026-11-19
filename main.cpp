#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/managers/input/trackpad/TrackpadGestures.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/ScrollMoveGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/WorkspaceSwipeGesture.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollTapeController.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace {
    constexpr eTrackpadGestureDirection DIRECTION       = TRACKPAD_GESTURE_DIR_HORIZONTAL;
    constexpr Input::ModifierMask       MODS            = Input::HL_MODIFIER_NONE;
    constexpr bool                      DISABLE_INHIBIT = false;

    struct SInstalled {
        size_t fingers = 0;
        float  scale   = 1.F;
    };

    HANDLE                              g_handle = nullptr;
    CHyprSignalListener                 g_reloadListener;
    std::optional<SInstalled>           g_installed;
    SP<Config::Values::CIntValue>       g_fingers;
    SP<Config::Values::CFloatValue>     g_scale;
    SP<Config::Values::CFloatValue>     g_workspaceScale;
    SP<Config::Values::CBoolValue>      g_leftIsForward;
    bool                                g_active    = false;
    std::string                         g_status    = "not installed";
    SP<IPC::Socket1::SCommand>          g_command;

    void                                log(const std::string& msg) {
        g_status = msg;
        LOG(Log::INFO, "[hypr-continuous-scrolling] {}", msg);
    }

    bool hasTiledNeighbor(bool forward) {
        const auto FOCUSED = Desktop::focusState()->window();
        if (!FOCUSED)
            return false;

        if (FOCUSED->isFloating())
            return true;

        const auto MONITOR = FOCUSED->m_monitor.lock();
        if (MONITOR && MONITOR->m_activeSpecialWorkspace)
            return true;

        const auto WORKSPACE = FOCUSED->m_workspace;
        if (!WORKSPACE)
            return true;

        const double CENTER = FOCUSED->layoutBox().middle().x;

        for (const auto& w : Desktop::windowState()->windows()) {
            if (!w || w == FOCUSED || !w->mapped() || w->isHidden() || w->isFloating() || w->m_workspace != WORKSPACE)
                continue;

            const double X = w->layoutBox().middle().x;
            if (forward ? X > CENTER : X < CENTER)
                return true;
        }

        return false;
    }

    struct STape {
        Layout::Tiled::CScrollingAlgorithm*   layout = nullptr;
        SP<Layout::Tiled::SScrollingData>     data;
        Layout::Tiled::CScrollTapeController* controller = nullptr;
        CBox                                  usable;
        double                                viewport = 0.0;
        double                                maxOffset = 0.0;
    };

    std::optional<STape> currentTape() {
        const auto MONITOR = Desktop::focusState()->monitor();
        if (!MONITOR)
            return std::nullopt;

        const auto WORKSPACE = MONITOR->m_activeSpecialWorkspace ? MONITOR->m_activeSpecialWorkspace : MONITOR->m_activeWorkspace;
        if (!WORKSPACE || !WORKSPACE->space())
            return std::nullopt;

        const auto ALGORITHM = WORKSPACE->space()->algorithm();
        if (!ALGORITHM || !ALGORITHM->tiledAlgo())
            return std::nullopt;

        STape tape;
        tape.layout = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(ALGORITHM->tiledAlgo().get());
        if (!tape.layout)
            return std::nullopt;

        const auto COLUMN = tape.layout->currentColumn();
        if (!COLUMN)
            return std::nullopt;

        tape.data = COLUMN->scrollingData.lock();
        if (!tape.data || !tape.data->controller)
            return std::nullopt;

        tape.controller = tape.data->controller.get();
        if (!tape.controller->isPrimaryHorizontal() || tape.controller->isReversed() || tape.controller->stripCount() < 2)
            return std::nullopt;

        tape.usable   = tape.layout->usableArea();
        tape.viewport = tape.usable.w;

        const double EXTENT = tape.controller->calculateMaxExtent(tape.usable, false);
        if (tape.viewport <= 0.0 || EXTENT <= tape.viewport)
            return std::nullopt;

        tape.maxOffset = EXTENT - tape.viewport;
        return tape;
    }

    void setTapeOffset(const STape& tape, double offset) {
        const double CURRENT = tape.controller->getOffset();
        if (std::abs(CURRENT - offset) > 0.5)
            tape.layout->moveTape(CURRENT - offset);
    }

    void clampTape() {
        const auto TAPE = currentTape();
        if (!TAPE)
            return;

        const double CURRENT = TAPE->controller->getOffset();
        setTapeOffset(*TAPE, std::clamp(CURRENT, 0.0, TAPE->maxOffset));
    }

    void centerTape() {
        const auto TAPE = currentTape();
        if (!TAPE)
            return;

        const auto  COLUMN  = TAPE->layout->currentColumn();
        const auto  INDEX   = COLUMN ? TAPE->data->idx(COLUMN) : -1;
        const auto& CONTROL = *TAPE->controller;

        if (INDEX < 0 || sc<size_t>(INDEX) >= CONTROL.stripCount())
            return;

        const double START = CONTROL.calculateStripStart(INDEX, TAPE->usable, false);
        const double SIZE  = CONTROL.calculateStripSize(INDEX, TAPE->usable, false);

        setTapeOffset(*TAPE, std::clamp(START - (TAPE->viewport - SIZE) / 2.0, 0.0, TAPE->maxOffset));
    }

    class CContinuousScrollGesture : public ITrackpadGesture {
      public:
        void begin(const STrackpadGestureBegin& e) override {
            ITrackpadGesture::begin(e);
            g_active = true;

            const bool FORWARD = (e.direction == TRACKPAD_GESTURE_DIR_LEFT) == g_leftIsForward->value();
            m_workspace        = (e.direction == TRACKPAD_GESTURE_DIR_LEFT || e.direction == TRACKPAD_GESTURE_DIR_RIGHT) && !hasTiledNeighbor(FORWARD);

            if (m_workspace)
                m_workspaceGesture.begin({.swipe = e.swipe, .pinch = e.pinch, .direction = e.direction, .scale = sc<float>(g_workspaceScale->value())});
            else
                m_scrollGesture.begin(e);
        }

        void update(const STrackpadGestureUpdate& e) override {
            if (m_workspace)
                m_workspaceGesture.update({.swipe = e.swipe, .pinch = e.pinch, .direction = e.direction, .scale = sc<float>(g_workspaceScale->value())});
            else {
                m_scrollGesture.update(e);
                clampTape();
            }
        }

        void end(const STrackpadGestureEnd& e) override {
            g_active = false;

            if (m_workspace)
                m_workspaceGesture.end({.swipe = e.swipe, .pinch = e.pinch, .direction = e.direction, .scale = sc<float>(g_workspaceScale->value())});
            else {
                m_scrollGesture.end(e);
                centerTape();
            }
        }

        bool isDirectionSensitive() override {
            return true;
        }

      private:
        bool                       m_workspace = false;
        CScrollMoveTrackpadGesture m_scrollGesture;
        CWorkspaceSwipeGesture     m_workspaceGesture;
    };

    void install() {
        g_installed.reset();

        if (!g_pTrackpadGestures) {
            log("trackpad gestures unavailable");
            return;
        }

        const size_t FINGERS = sc<size_t>(g_fingers->value());
        const float  SCALE   = sc<float>(g_scale->value());

        (void)g_pTrackpadGestures->removeGesture(FINGERS, DIRECTION, MODS, SCALE, DISABLE_INHIBIT);

        if (const auto ADDED = g_pTrackpadGestures->addGesture(makeUnique<CContinuousScrollGesture>(), FINGERS, DIRECTION, MODS, SCALE, DISABLE_INHIBIT); !ADDED) {
            log(std::format("could not add gesture ({})", ADDED.error()));
            return;
        }

        g_installed = SInstalled{.fingers = FINGERS, .scale = SCALE};
        log(std::format("gesture installed ({} fingers, scale {})", FINGERS, SCALE));
    }

    void safeInstall() {
        try {
            install();
        } catch (const std::exception& e) { log(std::format("install failed: {}", e.what())); }
    }
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_handle = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(handle, "[hypr-continuous-scrolling] Version mismatch, plugin not loaded", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hypr-continuous-scrolling] Version mismatch");
    }

    g_fingers        = makeShared<Config::Values::CIntValue>("plugin:continuous_scrolling:fingers", "Fingers for the swipe", 3, Config::Values::SIntValueOptions{.min = 2, .max = 5});
    g_scale          = makeShared<Config::Values::CFloatValue>("plugin:continuous_scrolling:scale", "Scroll speed inside a workspace", 1.F, Config::Values::SFloatValueOptions{.min = 0.1F, .max = 10.F});
    g_workspaceScale = makeShared<Config::Values::CFloatValue>("plugin:continuous_scrolling:workspace_scale", "Swipe speed when switching workspace", 1.F, Config::Values::SFloatValueOptions{.min = 0.1F, .max = 10.F});
    g_leftIsForward  = makeShared<Config::Values::CBoolValue>("plugin:continuous_scrolling:left_is_forward", "Swiping left moves to the next window", true);

    HyprlandAPI::addConfigValueV2(handle, g_fingers);
    HyprlandAPI::addConfigValueV2(handle, g_scale);
    HyprlandAPI::addConfigValueV2(handle, g_workspaceScale);
    HyprlandAPI::addConfigValueV2(handle, g_leftIsForward);

    g_reloadListener = Event::bus()->m_events.config.reloaded.listen([] { safeInstall(); });

    g_command = HyprlandAPI::registerHyprCtlCommand(handle,
                                                    IPC::Socket1::SCommand{
                                                        .name    = "continuousscrolling",
                                                        .match   = IPC::Socket1::COMMAND_MATCH_EXACT,
                                                        .handler = [](const IPC::Socket1::SRequest&) { return IPC::Socket1::SResponse{g_status + "\n"}; },
                                                    });

    safeInstall();

    return {"hypr-continuous-scrolling", "Scrolling layout trackpad swipe that continues into a workspace swipe at the edge", "yurareru", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_reloadListener.reset();

    if (g_command)
        HyprlandAPI::unregisterHyprCtlCommand(g_handle, g_command);
    g_command.reset();

    if (g_pTrackpadGestures && g_active)
        g_pTrackpadGestures->gestureEnd(IPointer::SSwipeEndEvent{.timeMs = 0, .cancelled = true});

    if (!g_pTrackpadGestures || !g_installed)
        return;

    const auto INSTALLED = *g_installed;
    if (g_pTrackpadGestures->removeGesture(INSTALLED.fingers, DIRECTION, MODS, INSTALLED.scale, DISABLE_INHIBIT))
        (void)g_pTrackpadGestures->addGesture(makeUnique<CScrollMoveTrackpadGesture>(), INSTALLED.fingers, DIRECTION, MODS, INSTALLED.scale, DISABLE_INHIBIT);

    g_installed.reset();
}
