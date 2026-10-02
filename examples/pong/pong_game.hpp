/**
 * @file pong_game.hpp
 * @brief 3D Pong game with Vulkan rendering and Qt6 UI
 * @version 1.0.0
 */

#pragma once

#include <memory>
#include <functional>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#ifdef WARPLOOM_HAS_QT_VULKAN
#include <QDialog>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QGroupBox>
#include <QFormLayout>
#include <QCheckBox>
#include <QMainWindow>
#include <QTimer>
#include <QPainter>
#include <QFont>
#include <QColor>
#endif

namespace Pong {

/**
 * @brief Game configuration structure
 */
struct GameConfig {
    // Ball settings
    float ball_radius{ 0.3f };
    float ball_speed{ 8.0f };

    // Paddle settings
    float paddle_width{ 0.5f };
    float paddle_height{ 2.0f };
    float paddle_speed{ 10.0f };

    // Field settings
    float field_width{ 20.0f };
    float field_height{ 10.0f };
    float field_depth{ 15.0f };

    // Game settings
    int win_score{ 10 };
    bool enable_ai{ true };
    float ai_difficulty{ 0.5f };
};

/**
 * @brief Game state structure
 */
struct GameState {
    // Ball state
    glm::vec3 ball_position{ 10.0f, 5.0f, 0.0f };
    glm::vec3 ball_velocity{ 8.0f, 4.0f, 0.0f };

    // Paddle states
    float paddle_left_y{ 5.0f };
    float paddle_right_y{ 5.0f };

    // Scores
    int left_score{ 0 };
    int right_score{ 0 };

    // Game state flags
    bool game_over{ false };
    bool paused{ false };
};

/**
 * @brief Input state structure
 */
struct InputState {
    bool up_pressed{ false };
    bool down_pressed{ false };
    bool w_pressed{ false };
    bool s_pressed{ false };
    bool escape_pressed{ false };
    bool space_pressed{ false };

    float mouse_x{ 0.0f };
    float mouse_y{ 0.0f };
    bool mouse_active{ false };
};

/**
 * @brief Statistics structure
 */
struct GameStatistics {
    uint32_t frame_count{ 0 };
    float fps{ 0.0f };
    float delta_time{ 0.0f };
    uint32_t total_frames{ 0 };
    float total_time{ 0.0f };
};

#ifdef WARPLOOM_HAS_QT_VULKAN
/**
 * @brief Qt6 configuration window for game settings
 */
class ConfigDialog : public QDialog {
    Q_OBJECT

public:
    explicit ConfigDialog(const GameConfig& current_config, QWidget* parent = nullptr);
    ~ConfigDialog() override = default;

    GameConfig get_config() const;

private Q_SLOTS:
    void on_reset_defaults();
    void on_apply();
    void on_ball_size_changed(int value);
    void on_paddle_size_changed(int value);
    void on_ball_speed_changed(int value);
    void on_paddle_speed_changed(int value);
    void on_win_score_changed(int value);
    void on_ai_difficulty_changed(int value);
    void on_enable_ai_toggled(int state);

private:
    void setup_ui();
    void update_preview();

    GameConfig m_config;
    QLabel* m_ball_size_label{ nullptr };
    QLabel* m_paddle_size_label{ nullptr };
    QLabel* m_ball_speed_label{ nullptr };
    QLabel* m_paddle_speed_label{ nullptr };
    QLabel* m_win_score_label{ nullptr };
    QLabel* m_ai_difficulty_label{ nullptr };
    QLabel* m_preview_label{ nullptr };
};

/**
 * @brief Qt6 statistics overlay widget
 */
class StatsOverlay : public QWidget {
    Q_OBJECT

public:
    explicit StatsOverlay(QWidget* parent = nullptr);
    ~StatsOverlay() override = default;

    void update_stats(const GameStatistics& stats);
    void update_score(int left_score, int right_score);
    void set_game_over(bool game_over);

private:
    void paintEvent(QPaintEvent* event) override;

    GameStatistics m_stats;
    int m_left_score{ 0 };
    int m_right_score{ 0 };
    bool m_game_over{ false };
};
#endif

/**
 * @brief 3D Pong game class
 */
class PongGame {
public:
    PongGame();
    ~PongGame();

    // Game lifecycle
    bool initialize(const GameConfig& config);
    void shutdown();
    void update(float delta_time);
    void render();

    // Input handling
    void handle_key_down(int key);
    void handle_key_up(int key);
    void handle_mouse_move(float x, float y);
    void handle_mouse_button(int button, bool pressed);

    // Game state
    void reset_game();
    void pause_game();
    void resume_game();
    bool is_game_over() const;
    bool is_paused() const;

    // Configuration
    void set_config(const GameConfig& config);
    GameConfig get_config() const;

    // Statistics
    GameStatistics get_statistics() const;

    // Game state access (for renderer)
    GameState get_game_state() const { return m_state; }

#ifdef WARPLOOM_HAS_QT_VULKAN
    // Qt6 UI integration
    void show_config_dialog();
    void set_stats_overlay(StatsOverlay* overlay);
#endif

private:
    void update_ball(float delta_time);
    void update_paddles(float delta_time);
    void check_collisions();
    void check_scoring();
    void update_statistics(float delta_time);

    // AI helper
    float calculate_ai_target();

    // Physics helpers
    bool check_ball_paddle_collision(const glm::vec3& ball_pos, const glm::vec3& paddle_pos,
                                     float paddle_width, float paddle_height);
    void reflect_ball_velocity(float normal_x, float normal_y);

    GameConfig m_config;
    GameState m_state;
    InputState m_input;
    GameStatistics m_stats;

#ifdef WARPLOOM_HAS_QT_VULKAN
    StatsOverlay* m_stats_overlay{ nullptr };
#endif
};

} // namespace Pong
