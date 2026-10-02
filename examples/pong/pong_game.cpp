/**
 * @file pong_game.cpp
 * @brief 3D Pong game implementation with Vulkan rendering and Qt6 UI
 * @version 1.0.0
 */

#include "pong_game.hpp"
#include <algorithm>
#include <cmath>
#include <random>
#include "engine/logging/Log.hpp"

#ifdef WARPLOOM_HAS_QT_VULKAN
#include <QMessageBox>
#include <QApplication>
#include <QKeyEvent>
#include <QGuiApplication>
#endif

namespace Pong {

// ============================================================================
// ConfigDialog Implementation
// ============================================================================

#ifdef WARPLOOM_HAS_QT_VULKAN
ConfigDialog::ConfigDialog(const GameConfig& current_config, QWidget* parent)
    : QDialog(parent), m_config(current_config) {

    setWindowTitle("Pong Game Settings");
    setMinimumWidth(400);
    setup_ui();
    update_preview();
}

GameConfig ConfigDialog::get_config() const {
    return m_config;
}

void ConfigDialog::setup_ui() {
    auto* main_layout = new QVBoxLayout(this);

    // Ball settings group
    auto* ball_group = new QGroupBox("Ball Settings", this);
    auto* ball_layout = new QFormLayout();

    m_ball_size_label = new QLabel(QString("Size: %1").arg(m_config.ball_radius, 0, 'f', 2), this);
    auto* ball_size_slider = new QSlider(Qt::Horizontal, this);
    ball_size_slider->setRange(10, 100);
    ball_size_slider->setValue(static_cast<int>(m_config.ball_radius * 100));
    connect(ball_size_slider, &QSlider::valueChanged, this, &ConfigDialog::on_ball_size_changed);

    m_ball_speed_label = new QLabel(QString("Speed: %1").arg(m_config.ball_speed, 0, 'f', 1), this);
    auto* ball_speed_slider = new QSlider(Qt::Horizontal, this);
    ball_speed_slider->setRange(1, 20);
    ball_speed_slider->setValue(static_cast<int>(m_config.ball_speed));
    connect(ball_speed_slider, &QSlider::valueChanged, this, &ConfigDialog::on_ball_speed_changed);

    ball_layout->addRow("Ball Size:", ball_size_slider);
    ball_layout->addRow("", m_ball_size_label);
    ball_layout->addRow("Ball Speed:", ball_speed_slider);
    ball_layout->addRow("", m_ball_speed_label);
    ball_group->setLayout(ball_layout);

    // Paddle settings group
    auto* paddle_group = new QGroupBox("Paddle Settings", this);
    auto* paddle_layout = new QFormLayout();

    m_paddle_size_label = new QLabel(QString("Size: %1x%2")
        .arg(m_config.paddle_width, 0, 'f', 2)
        .arg(m_config.paddle_height, 0, 'f', 2), this);
    auto* paddle_size_slider = new QSlider(Qt::Horizontal, this);
    paddle_size_slider->setRange(10, 50);
    paddle_size_slider->setValue(static_cast<int>(m_config.paddle_width * 10));
    connect(paddle_size_slider, &QSlider::valueChanged, this, &ConfigDialog::on_paddle_size_changed);

    m_paddle_speed_label = new QLabel(QString("Speed: %1").arg(m_config.paddle_speed, 0, 'f', 1), this);
    auto* paddle_speed_slider = new QSlider(Qt::Horizontal, this);
    paddle_speed_slider->setRange(1, 20);
    paddle_speed_slider->setValue(static_cast<int>(m_config.paddle_speed));
    connect(paddle_speed_slider, &QSlider::valueChanged, this, &ConfigDialog::on_paddle_speed_changed);

    paddle_layout->addRow("Paddle Size:", paddle_size_slider);
    paddle_layout->addRow("", m_paddle_size_label);
    paddle_layout->addRow("Paddle Speed:", paddle_speed_slider);
    paddle_layout->addRow("", m_paddle_speed_label);
    paddle_group->setLayout(paddle_layout);

    // Game settings group
    auto* game_group = new QGroupBox("Game Settings", this);
    auto* game_layout = new QFormLayout();

    m_win_score_label = new QLabel(QString("Win Score: %1").arg(m_config.win_score), this);
    auto* win_score_spin = new QSpinBox(this);
    win_score_spin->setRange(1, 50);
    win_score_spin->setValue(m_config.win_score);
    connect(win_score_spin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &ConfigDialog::on_win_score_changed);

    m_ai_difficulty_label = new QLabel(QString("AI Difficulty: %1").arg(m_config.ai_difficulty, 0, 'f', 2), this);
    auto* ai_difficulty_slider = new QSlider(Qt::Horizontal, this);
    ai_difficulty_slider->setRange(0, 100);
    ai_difficulty_slider->setValue(static_cast<int>(m_config.ai_difficulty * 100));
    connect(ai_difficulty_slider, &QSlider::valueChanged, this, &ConfigDialog::on_ai_difficulty_changed);

    auto* enable_ai_check = new QCheckBox("Enable AI Opponent", this);
    enable_ai_check->setChecked(m_config.enable_ai);
    connect(enable_ai_check, &QCheckBox::stateChanged, this, &ConfigDialog::on_enable_ai_toggled);

    game_layout->addRow("Win Score:", win_score_spin);
    game_layout->addRow("AI Difficulty:", ai_difficulty_slider);
    game_layout->addRow("", m_ai_difficulty_label);
    game_layout->addRow("", enable_ai_check);
    game_group->setLayout(game_layout);

    // Preview label
    m_preview_label = new QLabel("Game Preview", this);
    m_preview_label->setAlignment(Qt::AlignCenter);
    m_preview_label->setMinimumHeight(100);
    m_preview_label->setStyleSheet("QLabel { background-color: #2d2d2d; color: white; border-radius: 5px; padding: 10px; }");

    // Buttons
    auto* button_layout = new QHBoxLayout();
    auto* reset_button = new QPushButton("Reset Defaults", this);
    connect(reset_button, &QPushButton::clicked, this, &ConfigDialog::on_reset_defaults);

    auto* apply_button = new QPushButton("Apply", this);
    connect(apply_button, &QPushButton::clicked, this, &ConfigDialog::on_apply);

    auto* cancel_button = new QPushButton("Cancel", this);
    connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);

    button_layout->addWidget(reset_button);
    button_layout->addStretch();
    button_layout->addWidget(apply_button);
    button_layout->addWidget(cancel_button);

    // Add all to main layout
    main_layout->addWidget(ball_group);
    main_layout->addWidget(paddle_group);
    main_layout->addWidget(game_group);
    main_layout->addWidget(m_preview_label);
    main_layout->addLayout(button_layout);

    setLayout(main_layout);
}

void ConfigDialog::update_preview() {
    QString preview_text = QString(
        "Ball: R=%1, V=%2\n"
        "Paddle: %3x%4, S=%5\n"
        "Win Score: %6\n"
        "AI: %7"
    ).arg(m_config.ball_radius, 0, 'f', 2)
     .arg(m_config.ball_speed, 0, 'f', 1)
     .arg(m_config.paddle_width, 0, 'f', 2)
     .arg(m_config.paddle_height, 0, 'f', 2)
     .arg(m_config.paddle_speed, 0, 'f', 1)
     .arg(m_config.win_score)
     .arg(m_config.enable_ai ? QString("ON (%1)").arg(m_config.ai_difficulty, 0, 'f', 2) : "OFF");

    m_preview_label->setText(preview_text);
}

void ConfigDialog::on_reset_defaults() {
    m_config = GameConfig{};
    m_ball_size_label->setText(QString("Size: %1").arg(m_config.ball_radius, 0, 'f', 2));
    m_ball_speed_label->setText(QString("Speed: %1").arg(m_config.ball_speed, 0, 'f', 1));
    m_paddle_size_label->setText(QString("Size: %1x%2")
        .arg(m_config.paddle_width, 0, 'f', 2)
        .arg(m_config.paddle_height, 0, 'f', 2));
    m_paddle_speed_label->setText(QString("Speed: %1").arg(m_config.paddle_speed, 0, 'f', 1));
    m_win_score_label->setText(QString("Win Score: %1").arg(m_config.win_score));
    m_ai_difficulty_label->setText(QString("AI Difficulty: %1").arg(m_config.ai_difficulty, 0, 'f', 2));
    update_preview();
}

void ConfigDialog::on_apply() {
    accept();
}

void ConfigDialog::on_ball_size_changed(int value) {
    m_config.ball_radius = value / 100.0f;
    m_ball_size_label->setText(QString("Size: %1").arg(m_config.ball_radius, 0, 'f', 2));
    update_preview();
}

void ConfigDialog::on_paddle_size_changed(int value) {
    m_config.paddle_width = value / 10.0f;
    m_config.paddle_height = value / 5.0f;
    m_paddle_size_label->setText(QString("Size: %1x%2")
        .arg(m_config.paddle_width, 0, 'f', 2)
        .arg(m_config.paddle_height, 0, 'f', 2));
    update_preview();
}

void ConfigDialog::on_ball_speed_changed(int value) {
    m_config.ball_speed = static_cast<float>(value);
    m_ball_speed_label->setText(QString("Speed: %1").arg(m_config.ball_speed, 0, 'f', 1));
    update_preview();
}

void ConfigDialog::on_paddle_speed_changed(int value) {
    m_config.paddle_speed = static_cast<float>(value);
    m_paddle_speed_label->setText(QString("Speed: %1").arg(m_config.paddle_speed, 0, 'f', 1));
    update_preview();
}

void ConfigDialog::on_win_score_changed(int value) {
    m_config.win_score = value;
    m_win_score_label->setText(QString("Win Score: %1").arg(m_config.win_score));
    update_preview();
}

void ConfigDialog::on_ai_difficulty_changed(int value) {
    m_config.ai_difficulty = value / 100.0f;
    m_ai_difficulty_label->setText(QString("AI Difficulty: %1").arg(m_config.ai_difficulty, 0, 'f', 2));
    update_preview();
}

void ConfigDialog::on_enable_ai_toggled(int state) {
    m_config.enable_ai = (state == Qt::Checked);
    update_preview();
}

// ============================================================================
// StatsOverlay Implementation
// ============================================================================

StatsOverlay::StatsOverlay(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
}

void StatsOverlay::update_stats(const GameStatistics& stats) {
    m_stats = stats;
    update();
}

void StatsOverlay::update_score(int left_score, int right_score) {
    m_left_score = left_score;
    m_right_score = right_score;
    update();
}

void StatsOverlay::set_game_over(bool game_over) {
    m_game_over = game_over;
    update();
}

void StatsOverlay::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Semi-transparent background
    painter.fillRect(rect(), QColor(0, 0, 0, 180));

    // Set font
    QFont font("Arial", 14, QFont::Bold);
    painter.setFont(font);
    painter.setPen(QColor(255, 255, 255));

    // Draw FPS
    QString fps_text = QString("FPS: %1").arg(m_stats.fps, 0, 'f', 1);
    painter.drawText(10, 25, fps_text);

    // Draw Delta Time
    QString delta_text = QString("Delta: %1ms").arg(m_stats.delta_time * 1000.0f, 0, 'f', 2);
    painter.drawText(10, 45, delta_text);

    // Draw Frame Count
    QString frame_text = QString("Frames: %1").arg(m_stats.frame_count);
    painter.drawText(10, 65, frame_text);

    // Draw Score
    QFont score_font("Arial", 24, QFont::Bold);
    painter.setFont(score_font);

    QString score_text = QString("%1 - %2").arg(m_left_score).arg(m_right_score);
    QRect score_rect = rect();
    score_rect.setHeight(50);
    painter.drawText(score_rect, Qt::AlignCenter, score_text);

    // Draw Game Over message
    if (m_game_over) {
        QFont game_over_font("Arial", 36, QFont::Bold);
        painter.setFont(game_over_font);
        painter.setPen(QColor(255, 100, 100));

        QString winner_text;
        if (m_left_score >= m_right_score) {
            winner_text = "LEFT PLAYER WINS!";
        } else {
            winner_text = "RIGHT PLAYER WINS!";
        }

        QRect game_over_rect = rect();
        game_over_rect.setTop(game_over_rect.top() + 100);
        painter.drawText(game_over_rect, Qt::AlignCenter, winner_text);

        QString restart_text = "Press SPACE to restart";
        QFont restart_font("Arial", 18);
        painter.setFont(restart_font);
        painter.setPen(QColor(255, 255, 255));
        QRect restart_rect = rect();
        restart_rect.setTop(game_over_rect.top() + 150);
        painter.drawText(restart_rect, Qt::AlignCenter, restart_text);
    }
}
#endif

// ============================================================================
// PongGame Implementation
// ============================================================================

PongGame::PongGame() {
    omnicpp::log::info("PongGame: Constructor called");
}

PongGame::~PongGame() {
    shutdown();
    omnicpp::log::info("PongGame: Destructor called");
}

bool PongGame::initialize(const GameConfig& config) {
    omnicpp::log::info("PongGame: Initializing with config");
    omnicpp::log::info("  Ball radius: {}", config.ball_radius);
    omnicpp::log::info("  Ball speed: {}", config.ball_speed);
    omnicpp::log::info("  Paddle size: {}x{}", config.paddle_width, config.paddle_height);
    omnicpp::log::info("  Paddle speed: {}", config.paddle_speed);
    omnicpp::log::info("  Win score: {}", config.win_score);
    omnicpp::log::info("  AI enabled: {}", config.enable_ai);
    omnicpp::log::info("  AI difficulty: {}", config.ai_difficulty);

    m_config = config;
    reset_game();

    omnicpp::log::info("PongGame: Initialized successfully");
    return true;
}

void PongGame::shutdown() {
    omnicpp::log::info("PongGame: Shutting down");
}

void PongGame::update(float delta_time) {
    if (m_state.game_over) {
        // Check for restart input
        if (m_input.space_pressed) {
            reset_game();
        }
        return;
    }

    if (m_state.paused) {
        return;
    }

    // Update statistics
    update_statistics(delta_time);

    // Update game physics
    update_ball(delta_time);
    update_paddles(delta_time);
    check_collisions();
    check_scoring();
}

void PongGame::render() {
    // Rendering is handled by the Vulkan renderer
    // This method can be used to update rendering state
}

// Key constants - matching Qt::Key values
// Using integer constants to avoid Qt dependency in game logic
namespace {
constexpr int KEY_UP = 0x01000020;
constexpr int KEY_DOWN = 0x01000022;
constexpr int KEY_W = 0x57;      // 'W' key
constexpr int KEY_S = 0x53;      // 'S' key
constexpr int KEY_ESCAPE = 0x01000000;
constexpr int KEY_SPACE = 0x20;  // Space bar
}

void PongGame::handle_key_down(int key) {
    switch (key) {
        case KEY_UP:
            m_input.up_pressed = true;
            break;
        case KEY_DOWN:
            m_input.down_pressed = true;
            break;
        case KEY_W:
            m_input.w_pressed = true;
            break;
        case KEY_S:
            m_input.s_pressed = true;
            break;
        case KEY_ESCAPE:
            m_input.escape_pressed = true;
            break;
        case KEY_SPACE:
            m_input.space_pressed = true;
            break;
        default:
            break;
    }
}

void PongGame::handle_key_up(int key) {
    switch (key) {
        case KEY_UP:
            m_input.up_pressed = false;
            break;
        case KEY_DOWN:
            m_input.down_pressed = false;
            break;
        case KEY_W:
            m_input.w_pressed = false;
            break;
        case KEY_S:
            m_input.s_pressed = false;
            break;
        case KEY_ESCAPE:
            m_input.escape_pressed = false;
            break;
        case KEY_SPACE:
            m_input.space_pressed = false;
            break;
        default:
            break;
    }
}

void PongGame::handle_mouse_move(float x, float y) {
    m_input.mouse_x = x;
    m_input.mouse_y = y;
    m_input.mouse_active = true;
}

void PongGame::handle_mouse_button(int button, bool pressed) {
    if (button == 0) {
        // Left mouse button
        m_input.mouse_active = pressed;
    }
}

void PongGame::reset_game() {
    omnicpp::log::info("PongGame: Resetting game");

    // Reset ball position and velocity
    m_state.ball_position = glm::vec3(
        m_config.field_width / 2.0f,
        m_config.field_height / 2.0f,
        0.0f
    );

    // Randomize initial ball direction
    float angle = (static_cast<float>(rand()) / RAND_MAX) * 3.14159f * 2.0f;
    m_state.ball_velocity = glm::vec3(
        std::cos(angle) * m_config.ball_speed,
        std::sin(angle) * m_config.ball_speed,
        0.0f
    );

    // Reset paddle positions
    m_state.paddle_left_y = m_config.field_height / 2.0f;
    m_state.paddle_right_y = m_config.field_height / 2.0f;

    // Reset scores
    m_state.left_score = 0;
    m_state.right_score = 0;

    // Reset game state
    m_state.game_over = false;
    m_state.paused = false;

    // Reset statistics
    m_stats.frame_count = 0;
    m_stats.total_frames = 0;
    m_stats.total_time = 0.0f;

    omnicpp::log::info("PongGame: Game reset - Ball at ({}, {}), Velocity ({}, {})",
                 m_state.ball_position.x, m_state.ball_position.y,
                 m_state.ball_velocity.x, m_state.ball_velocity.y);

#ifdef WARPLOOM_HAS_QT_VULKAN
    if (m_stats_overlay) {
        m_stats_overlay->update_score(0, 0);
        m_stats_overlay->set_game_over(false);
    }
#endif
}

void PongGame::pause_game() {
    m_state.paused = true;
    omnicpp::log::info("PongGame: Game paused");
}

void PongGame::resume_game() {
    m_state.paused = false;
    omnicpp::log::info("PongGame: Game resumed");
}

bool PongGame::is_game_over() const {
    return m_state.game_over;
}

bool PongGame::is_paused() const {
    return m_state.paused;
}

void PongGame::set_config(const GameConfig& config) {
    m_config = config;
    omnicpp::log::info("PongGame: Config updated");
}

GameConfig PongGame::get_config() const {
    return m_config;
}

GameStatistics PongGame::get_statistics() const {
    return m_stats;
}

#ifdef WARPLOOM_HAS_QT_VULKAN
void PongGame::show_config_dialog() {
    ConfigDialog dialog(m_config, nullptr);
    if (dialog.exec() == QDialog::Accepted) {
        m_config = dialog.get_config();
        omnicpp::log::info("PongGame: Config updated from dialog");
        reset_game();
    }
}

void PongGame::set_stats_overlay(StatsOverlay* overlay) {
    m_stats_overlay = overlay;
}
#endif

void PongGame::update_ball(float delta_time) {
    // Update ball position based on velocity
    m_state.ball_position += m_state.ball_velocity * delta_time;

    // Check for ball out of bounds (top/bottom walls)
    float ball_top = m_state.ball_position.y + m_config.ball_radius;
    float ball_bottom = m_state.ball_position.y - m_config.ball_radius;

    if (ball_top >= m_config.field_height) {
        m_state.ball_position.y = m_config.field_height - m_config.ball_radius;
        m_state.ball_velocity.y = -m_state.ball_velocity.y;
    } else if (ball_bottom <= 0.0f) {
        m_state.ball_position.y = m_config.ball_radius;
        m_state.ball_velocity.y = -m_state.ball_velocity.y;
     }
}

float PongGame::calculate_ai_target() {
    // Only react when ball is moving towards AI (positive x velocity)
    if (m_state.ball_velocity.x <= 0.1f) {
        // Ball moving away or too slow, return to center
        return m_config.field_height / 2.0f;
    }
    
    // Predict where the ball will intersect with the right paddle line
    float paddle_x = m_config.field_width - 1.0f;
    float time_to_reach = (paddle_x - m_state.ball_position.x) / m_state.ball_velocity.x;
    
    // Clamp time_to_reach to prevent huge values
    time_to_reach = std::min(time_to_reach, 5.0f);
    
    // Calculate predicted y position
    float predicted_y = m_state.ball_position.y + m_state.ball_velocity.y * time_to_reach;
    
    // Account for wall bounces
    int bounces = 0;
    while ((predicted_y < 0.0f || predicted_y > m_config.field_height) && bounces < 10) {
        if (predicted_y < 0.0f) {
            predicted_y = -predicted_y;
        } else if (predicted_y > m_config.field_height) {
            predicted_y = 2.0f * m_config.field_height - predicted_y;
        }
        bounces++;
    }
    
    // Clamp to field bounds
    predicted_y = std::clamp(predicted_y, 0.0f, m_config.field_height);
    
    // Add some prediction error based on difficulty (lower difficulty = more error)
    // Use simple deterministic error based on frame count to avoid random_device issues
    static int error_counter = 0;
    error_counter++;
    float prediction_error = ((error_counter % 100) / 50.0f - 1.0f) * (1.0f - m_config.ai_difficulty) * 2.0f;
    
    return predicted_y + prediction_error;
}

void PongGame::update_paddles(float delta_time) {
    float paddle_move_speed = m_config.paddle_speed * delta_time;

    // Update left paddle (player controlled)
    if (m_input.w_pressed) {
        m_state.paddle_left_y += paddle_move_speed;
    }
    if (m_input.s_pressed) {
        m_state.paddle_left_y -= paddle_move_speed;
    }

    // Update right paddle (AI controlled)
    if (m_config.enable_ai) {
        // Enhanced AI with prediction and human-like behavior
        float target_y = calculate_ai_target();
        
        // Add reaction delay based on difficulty (lower difficulty = more delay)
        static float ai_reaction_timer = 0.0f;
        float reaction_delay = 0.1f * (1.0f - m_config.ai_difficulty);
        ai_reaction_timer += delta_time;
        
        if (ai_reaction_timer >= reaction_delay) {
            ai_reaction_timer = 0.0f;
            
            float diff = target_y - m_state.paddle_right_y;
            
            // AI speed varies with difficulty
            float ai_speed = paddle_move_speed * (0.5f + m_config.ai_difficulty * 1.5f);
            
            // Add some randomness to make AI beatable
            static std::random_device rd;
            static std::mt19937 gen(rd());
            std::uniform_real_distribution<float> noise(-0.3f, 0.3f);
            float random_offset = noise(gen) * (1.0f - m_config.ai_difficulty);
            
            diff += random_offset;
            
            if (std::abs(diff) > 0.1f) {
                float move_amount = std::min(ai_speed, std::abs(diff));
                m_state.paddle_right_y += (diff > 0.0f ? move_amount : -move_amount);
            }
        }
    } else {
        // Right paddle also controlled by arrow keys
        if (m_input.up_pressed) {
            m_state.paddle_right_y += paddle_move_speed;
        }
        if (m_input.down_pressed) {
            m_state.paddle_right_y -= paddle_move_speed;
        }
    }

    // Clamp paddle positions to field bounds
    float paddle_half_height = m_config.paddle_height / 2.0f;
    m_state.paddle_left_y = std::clamp(
        m_state.paddle_left_y,
        paddle_half_height,
        m_config.field_height - paddle_half_height
    );
    m_state.paddle_right_y = std::clamp(
        m_state.paddle_right_y,
        paddle_half_height,
        m_config.field_height - paddle_half_height
    );
}

void PongGame::check_collisions() {
    // Left paddle collision (paddle at x=1.0)
    if (check_ball_paddle_collision(
            m_state.ball_position,
            glm::vec3(1.0f, m_state.paddle_left_y, 0.0f),
            m_config.paddle_width,
            m_config.paddle_height)) {

        m_state.ball_position.x = 1.0f + m_config.ball_radius;
        m_state.ball_velocity.x = std::abs(m_state.ball_velocity.x);
        omnicpp::log::debug("PongGame: Ball hit left paddle");
    }

    // Right paddle collision (paddle at x=19.0)
    if (check_ball_paddle_collision(
            m_state.ball_position,
            glm::vec3(m_config.field_width - 1.0f, m_state.paddle_right_y, 0.0f),
            m_config.paddle_width,
            m_config.paddle_height)) {

        m_state.ball_position.x = m_config.field_width - 1.0f - m_config.ball_radius;
        m_state.ball_velocity.x = -std::abs(m_state.ball_velocity.x);
        omnicpp::log::debug("PongGame: Ball hit right paddle");
    }
}

void PongGame::check_scoring() {
    // Ball out of bounds on left side
    if (m_state.ball_position.x <= 0.0f) {
        m_state.right_score++;
        omnicpp::log::info("PongGame: Right player scores! Score: {} - {}",
                     m_state.left_score, m_state.right_score);

        // Reset ball
        m_state.ball_position = glm::vec3(
            m_config.field_width / 2.0f,
            m_config.field_height / 2.0f,
            0.0f
        );

        // Serve ball to the left
        float angle = (static_cast<float>(rand()) / RAND_MAX) * 3.14159f / 2.0f + 3.14159f * 1.5f;
        m_state.ball_velocity = glm::vec3(
            std::cos(angle) * m_config.ball_speed,
            std::sin(angle) * m_config.ball_speed,
            0.0f
        );

#ifdef WARPLOOM_HAS_QT_VULKAN
        if (m_stats_overlay) {
            m_stats_overlay->update_score(m_state.left_score, m_state.right_score);
        }
#endif

        // Check for game over
        if (m_state.right_score >= m_config.win_score) {
            m_state.game_over = true;
            omnicpp::log::info("PongGame: Game over! Right player wins!");
#ifdef WARPLOOM_HAS_QT_VULKAN
            if (m_stats_overlay) {
                m_stats_overlay->set_game_over(true);
            }
#endif
        }
    }

    // Ball out of bounds on right side
    if (m_state.ball_position.x >= m_config.field_width) {
        m_state.left_score++;
        omnicpp::log::info("PongGame: Left player scores! Score: {} - {}",
                     m_state.left_score, m_state.right_score);

        // Reset ball
        m_state.ball_position = glm::vec3(
            m_config.field_width / 2.0f,
            m_config.field_height / 2.0f,
            0.0f
        );

        // Serve ball to the right
        float angle = (static_cast<float>(rand()) / RAND_MAX) * 3.14159f / 2.0f + 3.14159f * 0.5f;
        m_state.ball_velocity = glm::vec3(
            std::cos(angle) * m_config.ball_speed,
            std::sin(angle) * m_config.ball_speed,
            0.0f
        );

#ifdef WARPLOOM_HAS_QT_VULKAN
        if (m_stats_overlay) {
            m_stats_overlay->update_score(m_state.left_score, m_state.right_score);
        }
#endif

        // Check for game over
        if (m_state.left_score >= m_config.win_score) {
            m_state.game_over = true;
            omnicpp::log::info("PongGame: Game over! Left player wins!");
#ifdef WARPLOOM_HAS_QT_VULKAN
            if (m_stats_overlay) {
                m_stats_overlay->set_game_over(true);
            }
#endif
        }
    }
}

void PongGame::update_statistics(float delta_time) {
    m_stats.frame_count++;
    m_stats.total_frames++;
    m_stats.total_time += delta_time;

    // Calculate FPS every 60 frames
    if (m_stats.frame_count % 60 == 0) {
        m_stats.fps = 1.0f / delta_time;
    }

    m_stats.delta_time = delta_time;
}

bool PongGame::check_ball_paddle_collision(const glm::vec3& ball_pos, const glm::vec3& paddle_pos,
                                           float paddle_width, float paddle_height) {
    // Simple AABB collision detection
    float ball_left = ball_pos.x - m_config.ball_radius;
    float ball_right = ball_pos.x + m_config.ball_radius;
    float ball_top = ball_pos.y + m_config.ball_radius;
    float ball_bottom = ball_pos.y - m_config.ball_radius;

    float paddle_left = paddle_pos.x - paddle_width / 2.0f;
    float paddle_right = paddle_pos.x + paddle_width / 2.0f;
    float paddle_top = paddle_pos.y + paddle_height / 2.0f;
    float paddle_bottom = paddle_pos.y - paddle_height / 2.0f;

    // Check if ball intersects with paddle
    return (ball_left < paddle_right &&
            ball_right > paddle_left &&
            ball_top > paddle_bottom &&
            ball_bottom < paddle_top);
}

void PongGame::reflect_ball_velocity(float normal_x, float normal_y) {
    // Reflect velocity vector around normal
    float dot_product = m_state.ball_velocity.x * normal_x + m_state.ball_velocity.y * normal_y;
    m_state.ball_velocity.x = m_state.ball_velocity.x - 2.0f * dot_product * normal_x;
    m_state.ball_velocity.y = m_state.ball_velocity.y - 2.0f * dot_product * normal_y;
}

} // namespace PongGame
