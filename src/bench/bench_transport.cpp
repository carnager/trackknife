// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "quick/mpd_probe_controller.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "uicommon/line_slider.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace trackknife::bench {
namespace {

constexpr int transport_refresh_ms = 33;
constexpr int minimum_custom_buffer_ms = 10;
constexpr int maximum_custom_buffer_ms = 10'000;
constexpr auto buffer_profile_settings_key = "playback/buffer-profile";
constexpr auto buffer_capacity_settings_key = "playback/buffer-capacity-ms";
constexpr auto buffer_threshold_settings_key = "playback/buffer-start-threshold-ms";

struct PlaybackBufferPreference {
    QString profile;
    audio::PlaybackBufferDurationConfig config;
};

[[nodiscard]] QString bufferProfileLabel(const QString& profile) {
    if (profile == QStringLiteral("responsive")) {
        return QStringLiteral("Responsive");
    }
    if (profile == QStringLiteral("resilient")) {
        return QStringLiteral("Resilient");
    }
    if (profile == QStringLiteral("custom")) {
        return QStringLiteral("Custom");
    }
    return QStringLiteral("Balanced");
}

[[nodiscard]] PlaybackBufferPreference loadPlaybackBufferPreference() {
    QSettings settings;
    const auto profile =
        settings.value(QString::fromLatin1(buffer_profile_settings_key), QStringLiteral("balanced"))
            .toString();
    const auto profile_bytes = utf8Bytes(profile);
    if (const auto preset = audio::playback_buffer_preset_from_id(profile_bytes)) {
        return {.profile = profile, .config = audio::playback_buffer_preset_config(*preset)};
    }
    if (profile == QStringLiteral("custom")) {
        bool capacity_ok = false;
        bool threshold_ok = false;
        const auto capacity =
            settings.value(QString::fromLatin1(buffer_capacity_settings_key)).toInt(&capacity_ok);
        const auto threshold =
            settings.value(QString::fromLatin1(buffer_threshold_settings_key)).toInt(&threshold_ok);
        const audio::PlaybackBufferDurationConfig config{
            .capacity = std::chrono::milliseconds{capacity},
            .start_threshold = std::chrono::milliseconds{threshold},
        };
        if (capacity_ok && threshold_ok && capacity >= minimum_custom_buffer_ms &&
            capacity <= maximum_custom_buffer_ms &&
            audio::valid_local_audition_buffer_config(config)) {
            return {.profile = profile, .config = config};
        }
    }
    return {.profile = QStringLiteral("balanced"),
            .config = audio::playback_buffer_preset_config(audio::PlaybackBufferPreset::balanced)};
}

[[nodiscard]] bool playerActive(const audio::LocalAuditionState state) {
    return state == audio::LocalAuditionState::buffering ||
           state == audio::LocalAuditionState::playing ||
           state == audio::LocalAuditionState::draining;
}

[[nodiscard]] core::Result<void> load_and_play(audio::LocalAuditionService& player,
                                               const LocalTrackSource& source) {
    return source.segment ? player.load_selected_segment_and_play(source.raw_path, source.selection,
                                                                  *source.segment)
                          : player.load_selected_and_play(source.raw_path, source.selection);
}

[[nodiscard]] core::Result<void> queue_gapless(audio::LocalAuditionService& player,
                                               const LocalTrackSource& source) {
    return source.segment ? player.queue_gapless_next_selected_segment(
                                source.raw_path, source.selection, *source.segment)
                          : player.queue_gapless_next_selected(source.raw_path, source.selection);
}

[[nodiscard]] LocalTrackSource source_from_snapshot(const audio::LocalAuditionSnapshot& snapshot) {
    return LocalTrackSource{.raw_path = snapshot.raw_path,
                            .selection = snapshot.selection,
                            .segment = snapshot.segment};
}

[[nodiscard]] std::optional<LocalTrackSource>
queued_source_from_snapshot(const audio::LocalAuditionSnapshot& snapshot) {
    if (snapshot.next_raw_path.empty()) {
        return std::nullopt;
    }
    return LocalTrackSource{.raw_path = snapshot.next_raw_path,
                            .selection = snapshot.next_selection,
                            .segment = snapshot.next_segment};
}

} // namespace

BenchMainWindow::BenchMainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Trackbench"));
    resize(1100, 720);
    setAcceptDrops(true);

    const auto buffer_preference = loadPlaybackBufferPreference();
    selected_buffer_profile_ = buffer_preference.profile;
    audio::LocalAuditionConfig player_config;
    player_config.buffer = buffer_preference.config;
    if (auto player = audio::LocalAuditionService::create(std::move(player_config)); player) {
        player_storage_ = std::move(*player);
        player_ = player_storage_.get();
    }

    buildWorkspace();
    buildTransport();
    connect(&metadata_operation_watcher_, &QFutureWatcherBase::finished, this,
            &BenchMainWindow::finishMetadataOperationJob);
    initializePersistence();

    if (player_ != nullptr) {
        static_cast<void>(player_->refresh_output_devices());
    } else {
        statusBar()->showMessage(
            QStringLiteral("Local playback unavailable: the audio worker failed to start"));
    }
    transport_timer_ = new QTimer(this);
    transport_timer_->setInterval(transport_refresh_ms);
    connect(transport_timer_, &QTimer::timeout, this, &BenchMainWindow::refreshTransport);
    transport_timer_->start();
    refreshActiveContext();
    refreshTransport();
}

BenchMainWindow::~BenchMainWindow() { stopBackgroundWork(); }

void BenchMainWindow::buildTransport() {
    auto* bar = addToolBar(QStringLiteral("Transport"));
    bar->setObjectName(QStringLiteral("bench-transport"));
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setIconSize(QSize{18, 18});
    bar->setToolButtonStyle(Qt::ToolButtonIconOnly);

    previous_action_ = new QAction(style()->standardIcon(QStyle::SP_MediaSkipBackward),
                                   QStringLiteral("Previous"), this);
    connect(previous_action_, &QAction::triggered, this, [this] {
        if (isMpdContext()) {
            mpd_controller_->previous();
        } else {
            playAdjacent(-1);
        }
    });
    play_pause_action_ =
        new QAction(style()->standardIcon(QStyle::SP_MediaPlay), QStringLiteral("Play"), this);
    play_pause_action_->setShortcut(Qt::Key_Space);
    play_pause_action_->setShortcutContext(Qt::ApplicationShortcut);
    connect(play_pause_action_, &QAction::triggered, this, &BenchMainWindow::togglePlayPause);
    stop_action_ =
        new QAction(style()->standardIcon(QStyle::SP_MediaStop), QStringLiteral("Stop"), this);
    connect(stop_action_, &QAction::triggered, this, [this] {
        if (isMpdContext()) {
            mpd_controller_->stop();
        } else if (player_ != nullptr) {
            static_cast<void>(player_->stop());
        }
    });
    next_action_ = new QAction(style()->standardIcon(QStyle::SP_MediaSkipForward),
                               QStringLiteral("Next"), this);
    connect(next_action_, &QAction::triggered, this, [this] {
        if (isMpdContext()) {
            mpd_controller_->next();
        } else {
            playAdjacent(1);
        }
    });

    auto* header = new QWidget(bar);
    header->setObjectName(QStringLiteral("bench-player-header"));
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* header_layout = new QGridLayout(header);
    header_layout->setContentsMargins(6, 3, 6, 3);
    header_layout->setHorizontalSpacing(6);
    header_layout->setVerticalSpacing(0);
    header_layout->setColumnStretch(2, 1);

    auto* transport = new QWidget(header);
    transport->setObjectName(QStringLiteral("bench-transport-buttons"));
    auto* transport_layout = new QHBoxLayout(transport);
    transport_layout->setContentsMargins(0, 0, 2, 0);
    transport_layout->setSpacing(1);
    const auto add_transport_button = [transport, transport_layout](QAction* action) {
        auto* button = new QToolButton(transport);
        button->setDefaultAction(action);
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setFixedSize(26, 26);
        button->setIconSize(QSize{18, 18});
        transport_layout->addWidget(button);
    };
    add_transport_button(previous_action_);
    add_transport_button(play_pause_action_);
    add_transport_button(stop_action_);
    add_transport_button(next_action_);
    header_layout->addWidget(transport, 1, 0, Qt::AlignVCenter);

    auto* track_display = new QWidget(header);
    track_display->setObjectName(QStringLiteral("bench-track-display"));
    track_display->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto* track_display_layout = new QVBoxLayout(track_display);
    track_display_layout->setContentsMargins(0, 0, 0, 1);
    track_display_layout->setSpacing(0);

    now_playing_ = new QLabel(track_display);
    now_playing_->setObjectName(QStringLiteral("bench-now-playing"));
    now_playing_->setTextFormat(Qt::PlainText);
    now_playing_->setAccessibleName(QStringLiteral("Current artist and title"));
    now_playing_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    now_playing_->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    track_display_layout->addWidget(now_playing_);

    now_playing_context_ = new QLabel(track_display);
    now_playing_context_->setObjectName(QStringLiteral("bench-now-playing-context"));
    now_playing_context_->setTextFormat(Qt::PlainText);
    now_playing_context_->setAccessibleName(QStringLiteral("Current album and date"));
    now_playing_context_->setForegroundRole(QPalette::PlaceholderText);
    now_playing_context_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    now_playing_context_->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    track_display_layout->addWidget(now_playing_context_);
    header_layout->addWidget(track_display, 0, 2);

    elapsed_ = new QLabel(QStringLiteral("0:00"), header);
    elapsed_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    elapsed_->setFixedWidth(elapsed_->fontMetrics().horizontalAdvance(QStringLiteral("00:00:00")));
    header_layout->addWidget(elapsed_, 1, 1, Qt::AlignVCenter);
    seek_ = new ui::LineSlider(header);
    seek_->setObjectName(QStringLiteral("bench-seek"));
    seek_->setAccessibleName(QStringLiteral("Playback position"));
    seek_->setMinimumWidth(200);
    seek_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(seek_, &QSlider::sliderPressed, this, [this] { seeking_ = true; });
    connect(seek_, &QSlider::sliderReleased, this, [this] {
        seeking_ = false;
        seekToMs(seek_->value());
    });
    header_layout->addWidget(seek_, 1, 2, Qt::AlignVCenter);
    duration_ = new QLabel(QStringLiteral("0:00"), header);
    duration_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    duration_->setFixedWidth(
        duration_->fontMetrics().horizontalAdvance(QStringLiteral("00:00:00")));
    header_layout->addWidget(duration_, 1, 3, Qt::AlignVCenter);

    volume_ = new ui::LineSlider(header);
    volume_->setObjectName(QStringLiteral("bench-volume"));
    volume_->setAccessibleName(QStringLiteral("Volume"));
    volume_->setRange(0, 100);
    volume_->setValue(100);
    volume_->setFixedWidth(104);
    volume_->setToolTip(QStringLiteral("Volume"));
    connect(volume_, &QSlider::sliderPressed, this, [this] { changing_volume_ = true; });
    connect(volume_, &QSlider::sliderReleased, this, [this] { changing_volume_ = false; });
    connect(volume_, &QSlider::valueChanged, this, [this](const int value) {
        if (isMpdContext()) {
            mpd_controller_->setVolume(value);
        } else if (player_ != nullptr) {
            static_cast<void>(player_->set_volume_percent(value));
        }
    });
    header_layout->addWidget(volume_, 1, 4, Qt::AlignVCenter);

    device_button_ = new QToolButton(header);
    device_button_->setObjectName(QStringLiteral("bench-device"));
    device_button_->setIcon(QIcon::fromTheme(QStringLiteral("audio-speakers"),
                                             style()->standardIcon(QStyle::SP_ComputerIcon)));
    device_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    device_button_->setAutoRaise(true);
    device_button_->setFixedSize(26, 26);
    device_button_->setIconSize(QSize{18, 18});
    device_button_->setPopupMode(QToolButton::InstantPopup);
    device_button_->setAccessibleName(QStringLiteral("Audio output device"));
    device_menu_ = new QMenu(device_button_);
    device_menu_->setObjectName(QStringLiteral("bench-device-menu"));
    device_group_ = new QActionGroup(device_menu_);
    device_group_->setExclusive(true);
    device_button_->setMenu(device_menu_);
    rebuildDeviceMenu();
    header_layout->addWidget(device_button_, 1, 5, Qt::AlignVCenter);
    bar->addWidget(header);

    auto* playback_menu = menuBar()->addMenu(QStringLiteral("&Playback"));
    playback_menu->addAction(play_pause_action_);
    playback_menu->addAction(stop_action_);
    playback_menu->addAction(previous_action_);
    playback_menu->addAction(next_action_);
    playback_menu->addSeparator();

    buffer_menu_ = playback_menu->addMenu(QStringLiteral("Playback buffer"));
    buffer_menu_->setObjectName(QStringLiteral("bench-buffer-menu"));
    buffer_group_ = new QActionGroup(buffer_menu_);
    buffer_group_->setExclusive(true);
    const auto add_buffer_preset = [this](const QString& label,
                                          const audio::PlaybackBufferPreset preset) {
        auto* action = buffer_menu_->addAction(label);
        const auto id = audio::playback_buffer_preset_id(preset);
        const auto profile = QString::fromLatin1(id.data(), static_cast<qsizetype>(id.size()));
        const auto config = audio::playback_buffer_preset_config(preset);
        action->setObjectName(QStringLiteral("action-buffer-%1").arg(profile));
        action->setData(profile);
        action->setCheckable(true);
        action->setToolTip(QStringLiteral("%1 ms capacity; playback starts at %2 ms")
                               .arg(config.capacity.count())
                               .arg(config.start_threshold.count()));
        buffer_group_->addAction(action);
        connect(action, &QAction::triggered, this, [this, profile, config] {
            configurePlaybackBuffer(profile, static_cast<int>(config.capacity.count()),
                                    static_cast<int>(config.start_threshold.count()));
        });
    };
    add_buffer_preset(QStringLiteral("Responsive"), audio::PlaybackBufferPreset::responsive);
    add_buffer_preset(QStringLiteral("Balanced"), audio::PlaybackBufferPreset::balanced);
    add_buffer_preset(QStringLiteral("Resilient"), audio::PlaybackBufferPreset::resilient);
    buffer_menu_->addSeparator();
    auto* custom_buffer = buffer_menu_->addAction(QStringLiteral("Custom…"));
    custom_buffer->setObjectName(QStringLiteral("action-buffer-custom"));
    custom_buffer->setData(QStringLiteral("custom"));
    custom_buffer->setCheckable(true);
    buffer_group_->addAction(custom_buffer);
    connect(custom_buffer, &QAction::triggered, this,
            &BenchMainWindow::showCustomPlaybackBufferDialog);
    refreshPlaybackBufferChecks();

    auto* refresh_devices = playback_menu->addAction(QStringLiteral("Refresh audio devices"));
    connect(refresh_devices, &QAction::triggered, this, [this] {
        if (player_ != nullptr) {
            static_cast<void>(player_->refresh_output_devices());
        }
    });
}

void BenchMainWindow::configurePlaybackBuffer(const QString& profile, const int capacity_ms,
                                              const int start_threshold_ms) {
    const audio::PlaybackBufferDurationConfig config{
        .capacity = std::chrono::milliseconds{capacity_ms},
        .start_threshold = std::chrono::milliseconds{start_threshold_ms},
    };
    if (!audio::valid_local_audition_buffer_config(config)) {
        statusBar()->showMessage(QStringLiteral("Invalid playback buffer values"), 5'000);
        refreshPlaybackBufferChecks();
        return;
    }

    auto active_buffer = std::optional<audio::PlaybackBufferDurationConfig>{};
    if (player_ != nullptr) {
        active_buffer = player_->snapshot().active_buffer;
        if (auto changed = player_->set_buffer_config(config); !changed) {
            statusBar()->showMessage(QStringLiteral("Playback buffer unchanged: %1")
                                         .arg(displayText(changed.error().message)),
                                     5'000);
            refreshPlaybackBufferChecks();
            return;
        }
    }

    selected_buffer_profile_ = profile;
    QSettings settings;
    settings.setValue(QString::fromLatin1(buffer_profile_settings_key), profile);
    settings.setValue(QString::fromLatin1(buffer_capacity_settings_key), capacity_ms);
    settings.setValue(QString::fromLatin1(buffer_threshold_settings_key), start_threshold_ms);
    settings.sync();
    refreshPlaybackBufferChecks();

    const bool pending = active_buffer && *active_buffer != config;
    statusBar()->showMessage(
        QStringLiteral("%1 buffer · %2 ms capacity · %3 ms start%4")
            .arg(bufferProfileLabel(profile))
            .arg(capacity_ms)
            .arg(start_threshold_ms)
            .arg(pending ? QStringLiteral(" · applies next track") : QString{}),
        5'000);
}

void BenchMainWindow::showCustomPlaybackBufferDialog() {
    const auto current = loadPlaybackBufferPreference().config;

    QDialog dialog{this};
    dialog.setObjectName(QStringLiteral("bench-custom-buffer-dialog"));
    dialog.setWindowTitle(QStringLiteral("Custom playback buffer"));
    auto* form = new QFormLayout(&dialog);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto* capacity = new QSpinBox(&dialog);
    capacity->setObjectName(QStringLiteral("bench-buffer-capacity"));
    capacity->setRange(minimum_custom_buffer_ms, maximum_custom_buffer_ms);
    capacity->setSuffix(QStringLiteral(" ms"));
    capacity->setValue(static_cast<int>(current.capacity.count()));
    form->addRow(QStringLiteral("Capacity"), capacity);

    auto* threshold = new QSpinBox(&dialog);
    threshold->setObjectName(QStringLiteral("bench-buffer-start-threshold"));
    threshold->setRange(1, capacity->value());
    threshold->setSuffix(QStringLiteral(" ms"));
    threshold->setValue(
        std::min(static_cast<int>(current.start_threshold.count()), capacity->value()));
    form->addRow(QStringLiteral("Start playback at"), threshold);
    connect(capacity, &QSpinBox::valueChanged, threshold,
            [threshold](const int value) { threshold->setMaximum(value); });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);

    if (dialog.exec() == QDialog::Accepted) {
        configurePlaybackBuffer(QStringLiteral("custom"), capacity->value(), threshold->value());
    } else {
        refreshPlaybackBufferChecks();
    }
}

void BenchMainWindow::refreshPlaybackBufferChecks() {
    if (buffer_group_ == nullptr) {
        return;
    }
    for (auto* action : buffer_group_->actions()) {
        action->setChecked(action->data().toString() == selected_buffer_profile_);
    }
}

void BenchMainWindow::rebuildDeviceMenu() {
    device_menu_->clear();

    if (isMpdContext()) {
        device_group_->setExclusive(false);
        auto* output_model = mpd_controller_->outputModel();
        for (int row = 0; row < output_model->rowCount(); ++row) {
            const auto index = output_model->index(row, 0);
            const auto id = output_model->data(index, quick::MpdOutputModel::OutputIdRole).toUInt();
            const auto name = output_model->data(index, quick::MpdOutputModel::NameRole).toString();
            const auto enabled =
                output_model->data(index, quick::MpdOutputModel::EnabledRole).toBool();
            const auto primary =
                output_model->data(index, quick::MpdOutputModel::PrimaryRole).toBool();
            auto* action = device_menu_->addAction(name);
            action->setObjectName(QStringLiteral("action-mpd-output-%1").arg(id));
            action->setCheckable(true);
            action->setChecked(mpd_controller_->supportsExclusiveOutput() ? primary : enabled);
            action->setToolTip(
                output_model->data(index, quick::MpdOutputModel::DetailRole).toString());
            device_group_->addAction(action);
            connect(action, &QAction::triggered, this, [this, id, enabled] {
                if (mpd_controller_->supportsExclusiveOutput()) {
                    mpd_controller_->switchOutput(id);
                } else {
                    mpd_controller_->setOutputEnabled(id, !enabled);
                }
            });
        }
        if (device_menu_->isEmpty()) {
            auto* none = device_menu_->addAction(mpd_controller_->connected()
                                                     ? QStringLiteral("No MPD outputs")
                                                     : QStringLiteral("Connect to MPD"));
            none->setEnabled(false);
        }
        device_button_->setAccessibleName(QStringLiteral("MPD output"));
        return;
    }

    device_group_->setExclusive(true);
    device_button_->setAccessibleName(QStringLiteral("Audio output device"));

    const auto add_choice = [this](const QString& label, std::optional<std::string> target,
                                   const bool enabled = true) {
        auto* action = device_menu_->addAction(label);
        action->setCheckable(true);
        action->setChecked(target == selected_device_);
        action->setEnabled(enabled);
        device_group_->addAction(action);
        connect(action, &QAction::triggered, this, [this, target = std::move(target)] {
            if (player_ != nullptr) {
                static_cast<void>(player_->set_output_target(target));
            }
        });
        return action;
    };

    add_choice(QStringLiteral("System default"), std::nullopt);
    for (const auto& [name, description] : device_choices_) {
        add_choice(displayText(description.empty() ? name : description), name);
    }
    if (selected_device_ && std::ranges::none_of(device_choices_, [this](const auto& choice) {
            return choice.first == *selected_device_;
        })) {
        add_choice(QStringLiteral("%1 (unavailable)").arg(displayText(*selected_device_)),
                   selected_device_, false);
    }
    device_menu_->addSeparator();
    auto* refresh = device_menu_->addAction(QStringLiteral("Refresh audio devices"));
    refresh->setObjectName(QStringLiteral("action-refresh-audio-devices"));
    connect(refresh, &QAction::triggered, this, [this] {
        if (player_ != nullptr) {
            static_cast<void>(player_->refresh_output_devices());
        }
    });
}

void BenchMainWindow::playRow(ListTab& tab, const int row) {
    if (player_ == nullptr) {
        return;
    }
    const auto source = tab.model->source(row);
    if (source.raw_path.empty()) {
        return;
    }
    if (auto result = load_and_play(*player_, source); !result) {
        statusBar()->showMessage(
            QStringLiteral("Playback failed: %1").arg(displayText(result.error().message)), 5'000);
        return;
    }
    const auto id = QString::fromStdString(tab.document.id.to_string());
    if (playback_document_id_ != id) {
        if (auto* previous = tabForDocument(playback_document_id_); previous != nullptr) {
            previous->model->setCurrentSource({}, -1);
        }
    }
    playback_document_id_ = id;
    playback_row_ = row;
    playback_source_ = source;
    // A load was just dispatched; block auto-advance until the player state
    // leaves "ended" so the previous track's end cannot skip this one.
    advance_pending_ = true;
    last_requested_next_.reset();
    tab.model->setCurrentSource(source, row);
}

std::optional<std::pair<int, LocalTrackSource>>
BenchMainWindow::adjacentPlaybackRow(const int direction) {
    auto* tab = tabForDocument(playback_document_id_);
    if (tab == nullptr || playback_source_.raw_path.empty()) {
        return std::nullopt;
    }
    const auto row = tab->model->rowOfSource(playback_source_, playback_row_);
    if (row < 0) {
        return std::nullopt;
    }
    const auto adjacent = row + direction;
    if (adjacent < 0 || adjacent >= tab->model->rowCount()) {
        return std::nullopt;
    }
    return std::make_pair(adjacent, tab->model->source(adjacent));
}

void BenchMainWindow::playAdjacent(const int direction) {
    auto* tab = tabForDocument(playback_document_id_);
    const auto next = adjacentPlaybackRow(direction);
    if (tab == nullptr || !next || player_ == nullptr) {
        return;
    }
    if (auto result = load_and_play(*player_, next->second); result) {
        playback_row_ = next->first;
        playback_source_ = next->second;
        advance_pending_ = true;
        last_requested_next_.reset();
        tab->model->setCurrentSource(next->second, next->first);
    } else {
        statusBar()->showMessage(
            QStringLiteral("Playback failed: %1").arg(displayText(result.error().message)), 5'000);
    }
}

void BenchMainWindow::togglePlayPause() {
    if (isMpdContext()) {
        mpd_controller_->playPause();
        return;
    }
    if (player_ == nullptr) {
        return;
    }
    const auto snapshot = player_->snapshot();
    if (playerActive(snapshot.state)) {
        static_cast<void>(player_->pause());
    } else {
        static_cast<void>(player_->play());
    }
}

void BenchMainWindow::seekToMs(const qint64 position_ms) {
    if (isMpdContext()) {
        mpd_controller_->seekTo(position_ms);
        return;
    }
    if (player_ == nullptr) {
        return;
    }
    const auto snapshot = player_->snapshot();
    if (!snapshot.format || snapshot.format->sample_rate <= 0) {
        return;
    }
    static_cast<void>(player_->seek_to_sample(position_ms * snapshot.format->sample_rate / 1'000));
}

void BenchMainWindow::refreshTransport() {
    if (isMpdContext()) {
        refreshMpdTransport();
        return;
    }
    if (player_ == nullptr) {
        for (auto* action : {previous_action_, play_pause_action_, stop_action_, next_action_}) {
            action->setEnabled(false);
        }
        seek_->setEnabled(false);
        volume_->setEnabled(false);
        device_button_->setEnabled(false);
        return;
    }
    const auto snapshot = player_->snapshot();
    // Observable for offscreen tests and diagnostics.
    setProperty("trackbench-player-state", static_cast<int>(snapshot.state));

    // A consumed gapless takeover moves the anchors and highlight without any
    // load; the engine already plays the next row.
    if (snapshot.chain_transitions != last_chain_transitions_) {
        last_chain_transitions_ = snapshot.chain_transitions;
        last_requested_next_.reset();
        if (auto* tab = tabForDocument(playback_document_id_);
            tab != nullptr && !snapshot.raw_path.empty()) {
            const auto transitioned_source = source_from_snapshot(snapshot);
            const auto row = tab->model->rowOfSource(transitioned_source, playback_row_ + 1);
            if (row >= 0) {
                playback_row_ = row;
                playback_source_ = transitioned_source;
                tab->model->setCurrentSource(transitioned_source, row);
            }
        }
    }
    setProperty("trackbench-player-position", static_cast<qlonglong>(snapshot.position_sample));
    setProperty("trackbench-player-buffered", static_cast<qlonglong>(snapshot.buffered_frames));
    setProperty("trackbench-player-buffer-capacity-ms",
                static_cast<qlonglong>(snapshot.configured_buffer.capacity.count()));
    setProperty("trackbench-player-active-buffer-capacity-ms",
                snapshot.active_buffer
                    ? static_cast<qlonglong>(snapshot.active_buffer->capacity.count())
                    : static_cast<qlonglong>(-1));
    setProperty("trackbench-player-buffer-pending",
                snapshot.active_buffer && *snapshot.active_buffer != snapshot.configured_buffer);
    setProperty("trackbench-player-underruns", static_cast<qulonglong>(snapshot.underrun_count));
    setProperty("trackbench-player-callbacks",
                static_cast<qlonglong>(snapshot.output.callback_count));
    setProperty("trackbench-player-outputstate", static_cast<int>(snapshot.output.state));
    setProperty("trackbench-player-output-available", snapshot.output_target_available);
    setProperty("trackbench-player-output-suspended", snapshot.output_suspended);
    setProperty("trackbench-player-device-generation",
                static_cast<qulonglong>(snapshot.device_generation));
    setProperty("trackbench-player-default-output",
                snapshot.default_output_target ? displayText(*snapshot.default_output_target)
                                               : QString{});

    // List progression (ADR-0023): a finished track advances once to the next
    // row of its originating list; the end of the list stays "Ended".
    if (snapshot.state == audio::LocalAuditionState::ended) {
        // The guard stays set until the worker actually leaves "ended";
        // resetting it on dispatch would re-fire every timer tick while the
        // next source is still loading and race through the list.
        if (!advance_pending_) {
            advance_pending_ = true;
            if (const auto next = adjacentPlaybackRow(1)) {
                if (auto result = load_and_play(*player_, next->second); result) {
                    playback_row_ = next->first;
                    playback_source_ = next->second;
                    if (auto* tab = tabForDocument(playback_document_id_); tab != nullptr) {
                        tab->model->setCurrentSource(next->second, next->first);
                    }
                }
            }
        }
    } else if (snapshot.state == audio::LocalAuditionState::empty) {
        if (!playback_document_id_.isEmpty()) {
            if (auto* tab = tabForDocument(playback_document_id_); tab != nullptr) {
                tab->model->setCurrentSource({}, -1);
            }
            playback_document_id_.clear();
            playback_row_ = -1;
            playback_source_ = {};
        }
        advance_pending_ = false;
    } else if (snapshot.state != audio::LocalAuditionState::loading) {
        advance_pending_ = false;
    }

    const auto error = snapshot.error ? displayText(snapshot.error->message) : QString{};
    if (!error.isEmpty() && error != last_player_error_) {
        last_player_error_ = error;
        statusBar()->showMessage(QStringLiteral("Playback failed: %1").arg(error), 5'000);
    } else if (error.isEmpty()) {
        last_player_error_.clear();
    }

    const auto monitor_error = snapshot.device_monitor_error
                                   ? displayText(snapshot.device_monitor_error->message)
                                   : QString{};
    if (!monitor_error.isEmpty() && monitor_error != last_device_monitor_error_) {
        last_device_monitor_error_ = monitor_error;
        statusBar()->showMessage(
            QStringLiteral("Audio device monitoring failed: %1").arg(monitor_error), 5'000);
    } else if (monitor_error.isEmpty()) {
        last_device_monitor_error_.clear();
    }
    const auto recovery_error = snapshot.output_recovery_error
                                    ? displayText(snapshot.output_recovery_error->message)
                                    : QString{};
    if (!recovery_error.isEmpty() && recovery_error != last_output_recovery_error_) {
        last_output_recovery_error_ = recovery_error;
        statusBar()->showMessage(
            QStringLiteral("Audio output recovery failed: %1").arg(recovery_error), 5'000);
    } else if (recovery_error.isEmpty()) {
        last_output_recovery_error_.clear();
    }
    if (last_device_generation_ != 0U) {
        if (selected_device_available_ && !snapshot.output_target_available) {
            statusBar()->showMessage(QStringLiteral("Audio output unavailable · playback paused"),
                                     5'000);
        } else if (!selected_device_available_ && snapshot.output_target_available &&
                   !snapshot.output_suspended) {
            statusBar()->showMessage(
                QStringLiteral("Audio output available again · press Play to resume"), 5'000);
        } else if (!snapshot.output_target && default_device_ &&
                   snapshot.default_output_target != default_device_) {
            statusBar()->showMessage(QStringLiteral("System audio output changed"), 5'000);
        }
    }

    // Keep the engine's queued continuation in sync with the next list row so
    // transitions are gapless. Re-requests are throttled: the engine drops
    // the queue on seeks and silently rejects format changes, and the drain
    // fallback below covers rejected continuations.
    if (snapshot.format.has_value() && snapshot.state != audio::LocalAuditionState::failed &&
        snapshot.state != audio::LocalAuditionState::empty &&
        snapshot.state != audio::LocalAuditionState::loading) {
        const auto next = adjacentPlaybackRow(1);
        const auto desired = next ? std::optional{next->second} : std::nullopt;
        const auto desired_marker = desired.value_or(LocalTrackSource{});
        const auto queued = queued_source_from_snapshot(snapshot);
        const bool changed = !last_requested_next_ || *last_requested_next_ != desired_marker;
        const bool stale = desired != queued && (!next_request_timer_.isValid() ||
                                                 next_request_timer_.elapsed() > 1'000);
        if (desired != queued && (changed || stale)) {
            if (!desired) {
                static_cast<void>(player_->clear_gapless_next());
            } else {
                static_cast<void>(queue_gapless(*player_, *desired));
            }
            last_requested_next_ = desired_marker;
            next_request_timer_.start();
        }
    }

    const bool active = playerActive(snapshot.state);
    const bool source_ready = snapshot.format.has_value() &&
                              snapshot.state != audio::LocalAuditionState::loading &&
                              snapshot.state != audio::LocalAuditionState::failed;
    previous_action_->setEnabled(adjacentPlaybackRow(-1).has_value());
    next_action_->setEnabled(adjacentPlaybackRow(1).has_value());
    play_pause_action_->setEnabled(source_ready && snapshot.output_target_available &&
                                   !snapshot.output_suspended);
    play_pause_action_->setText(active ? QStringLiteral("Pause") : QStringLiteral("Play"));
    play_pause_action_->setIcon(
        style()->standardIcon(active ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    stop_action_->setEnabled(source_ready);

    if (snapshot.state == audio::LocalAuditionState::empty) {
        now_playing_->clear();
        now_playing_context_->clear();
        now_playing_->setToolTip({});
        now_playing_context_->setToolTip({});
    } else {
        const auto slash = snapshot.raw_path.find_last_of('/');
        const auto name = slash == std::string::npos || slash + 1U >= snapshot.raw_path.size()
                              ? snapshot.raw_path
                              : snapshot.raw_path.substr(slash + 1U);
        const auto fallback = QString::fromStdString(core::escape_raw_path(name));
        auto label = fallback;
        auto context = QString{};
        if (auto* tab = tabForDocument(playback_document_id_); tab != nullptr) {
            const auto row = tab->model->rowOfSource(source_from_snapshot(snapshot), playback_row_);
            if (row >= 0) {
                const auto& track = tab->model->rows()[static_cast<std::size_t>(row)];
                const auto title = track.title.empty() ? fallback : displayText(track.title);
                if (!track.artist.empty()) {
                    label = QStringLiteral("%1 — %2").arg(displayText(track.artist), title);
                } else {
                    label = title;
                }
                QStringList details;
                if (!track.album.empty()) {
                    details.push_back(displayText(track.album));
                }
                if (!track.date.empty()) {
                    details.push_back(displayText(track.date));
                }
                context = details.join(QStringLiteral(" · "));
            }
        }
        now_playing_->setText(label);
        now_playing_context_->setText(context);
        const auto path_tooltip = QString::fromStdString(core::escape_raw_path(snapshot.raw_path));
        now_playing_->setToolTip(path_tooltip);
        now_playing_context_->setToolTip(context.isEmpty() ? path_tooltip : context);
    }

    qint64 position_ms = 0;
    qint64 duration_ms = 0;
    if (snapshot.format && snapshot.format->sample_rate > 0) {
        position_ms = snapshot.position_sample * 1'000 / snapshot.format->sample_rate;
        if (snapshot.end_sample) {
            duration_ms = *snapshot.end_sample * 1'000 / snapshot.format->sample_rate;
        }
    }
    elapsed_->setText(formatTime(position_ms));
    duration_->setText(formatTime(duration_ms));
    const auto bounded = std::clamp<qint64>(duration_ms, 0, std::numeric_limits<int>::max());
    seek_->setEnabled(source_ready && bounded > 0);
    seek_->setRange(0, static_cast<int>(bounded));
    if (!seeking_) {
        const QSignalBlocker blocker{seek_};
        seek_->setValue(
            static_cast<int>(std::clamp<qint64>(position_ms, 0, std::numeric_limits<int>::max())));
    }
    volume_->setEnabled(true);
    if (!changing_volume_) {
        const QSignalBlocker blocker{volume_};
        volume_->setValue(snapshot.volume_percent);
    }

    std::vector<std::pair<std::string, std::string>> choices;
    choices.reserve(snapshot.devices.size());
    for (const auto& device : snapshot.devices) {
        choices.emplace_back(device.name, device.description);
    }
    const bool device_menu_changed = choices != device_choices_;
    const bool selection_changed = snapshot.output_target != selected_device_;
    const bool availability_changed =
        snapshot.output_target_available != selected_device_available_;
    const bool default_changed = snapshot.default_output_target != default_device_;
    device_choices_ = std::move(choices);
    selected_device_ = snapshot.output_target;
    default_device_ = snapshot.default_output_target;
    selected_device_available_ = snapshot.output_target_available;
    last_device_generation_ = snapshot.device_generation;
    if (device_menu_changed || selection_changed || availability_changed || default_changed) {
        rebuildDeviceMenu();
    }
    QString device_label = QStringLiteral("System default");
    if (selected_device_) {
        const auto found = std::ranges::find(device_choices_, *selected_device_,
                                             &std::pair<std::string, std::string>::first);
        device_label = found == device_choices_.end()
                           ? displayText(*selected_device_)
                           : displayText(found->second.empty() ? found->first : found->second);
    } else if (default_device_) {
        const auto found = std::ranges::find(device_choices_, *default_device_,
                                             &std::pair<std::string, std::string>::first);
        const auto default_label =
            found == device_choices_.end()
                ? displayText(*default_device_)
                : displayText(found->second.empty() ? found->first : found->second);
        device_label += QStringLiteral(" — %1").arg(default_label);
    }
    if (!snapshot.output_target_available) {
        device_label += QStringLiteral(" (unavailable)");
    }
    device_button_->setEnabled(true);
    const bool buffer_pending =
        snapshot.active_buffer && *snapshot.active_buffer != snapshot.configured_buffer;
    auto audio_tooltip =
        QStringLiteral("Audio output: %1\nBuffer: %2 · %3 ms capacity · %4 ms start%5\n"
                       "Underruns: %6")
            .arg(device_label)
            .arg(bufferProfileLabel(selected_buffer_profile_))
            .arg(snapshot.configured_buffer.capacity.count())
            .arg(snapshot.configured_buffer.start_threshold.count())
            .arg(buffer_pending ? QStringLiteral(" · applies next track") : QString{})
            .arg(snapshot.underrun_count);
    if (!snapshot.output_target_available) {
        audio_tooltip += QStringLiteral("\nPlayback is paused until an output is available");
    } else if (snapshot.output_suspended) {
        audio_tooltip += QStringLiteral("\nReconnecting the audio output");
    }
    if (snapshot.output.node_id) {
        audio_tooltip += QStringLiteral("\nPipeWire node: %1").arg(*snapshot.output.node_id);
    }
    device_button_->setToolTip(audio_tooltip);
    device_button_->setAccessibleDescription(device_label);
}

} // namespace trackknife::bench
