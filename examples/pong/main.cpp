/**
 * @file main.cpp
 * @brief 3D Pong game using OmniCpp engine with Vulkan rendering and Qt6 UI
 * @version 1.0.0
 */

#include "engine/graphics/renderer.hpp"
#include "engine/window/window_manager.hpp"
#include "pong_game.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstring>
#include "engine/logging/Log.hpp"

#ifdef WARPLOOM_HAS_QT_VULKAN
#include <QApplication>
#include <QPointer>
#include <QMainWindow>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTimer>
#include <QShortcut>
#include <QKeySequence>
#include <QMessageBox>
#include <QKeyEvent>
#include <QMouseEvent>
#endif

using namespace Pong;

// ============================================================================
// MainWindow class - Combines game window with Qt6 UI
// ============================================================================

#ifdef WARPLOOM_HAS_QT_VULKAN
class MainWindow : public QMainWindow {
public:
    explicit MainWindow(QWindow* vulkan_window, QWidget* parent = nullptr);
    ~MainWindow() override = default;

    void set_game(PongGame* game);
    void update_ui();

private:
    void on_game_loop();
    void on_show_config();
    void on_toggle_pause();

private:
    void setup_ui();
    void setup_shortcuts();
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

    QWindow* m_vulkan_window{ nullptr };
    PongGame* m_game{ nullptr };
    StatsOverlay* m_stats_overlay{ nullptr };
    QTimer* m_game_timer{ nullptr };

    // FPS tracking
    std::chrono::high_resolution_clock::time_point m_last_frame_time;
    uint32_t m_frame_count{ 0 };
    std::chrono::high_resolution_clock::time_point m_last_fps_time;
    uint32_t m_fps_frame_count{ 0 };
};

MainWindow::MainWindow(QWindow* vulkan_window, QWidget* parent)
    : QMainWindow(parent), m_vulkan_window(vulkan_window) {

    setWindowTitle("OmniCpp 3D Pong - Vulkan");
    setMinimumSize(800, 600);

    setup_ui();
    setup_shortcuts();

    // Create game timer for 60 FPS
    m_game_timer = new QTimer(this);
    m_game_timer->setInterval(16); // ~60 FPS
    connect(m_game_timer, &QTimer::timeout, this, &MainWindow::on_game_loop);
    m_game_timer->start();

    m_last_frame_time = std::chrono::high_resolution_clock::now();
    m_last_fps_time = m_last_frame_time;

    omnicpp::log::info("MainWindow: Created successfully");
}

void MainWindow::setup_ui() {
    // Create central widget
    auto* central_widget = new QWidget(this);
    setCentralWidget(central_widget);

    // Create main layout
    auto* main_layout = new QVBoxLayout(central_widget);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(0);

    // Create stats overlay
    m_stats_overlay = new StatsOverlay(central_widget);
    m_stats_overlay->setGeometry(0, 0, 800, 600);
    main_layout->addWidget(m_stats_overlay);

    // Create control panel at the bottom
    auto* control_panel = new QWidget(central_widget);
    control_panel->setMaximumHeight(60);
    control_panel->setStyleSheet("QWidget { background-color: #1a1a2a; }");

    auto* control_layout = new QHBoxLayout(control_panel);
    control_layout->setContentsMargins(10, 5, 10, 5);

    // Config button
    auto* config_button = new QPushButton("Settings", control_panel);
    config_button->setMinimumWidth(100);
    connect(config_button, &QPushButton::clicked, this, &MainWindow::on_show_config);
    control_layout->addWidget(config_button);

    // Pause button
    auto* pause_button = new QPushButton("Pause", control_panel);
    pause_button->setMinimumWidth(100);
    connect(pause_button, &QPushButton::clicked, this, &MainWindow::on_toggle_pause);
    control_layout->addWidget(pause_button);

    // Exit button
    auto* exit_button = new QPushButton("Exit", control_panel);
    exit_button->setMinimumWidth(100);
    connect(exit_button, &QPushButton::clicked, this, &QMainWindow::close);
    control_layout->addWidget(exit_button);

    control_layout->addStretch();

    main_layout->addWidget(control_panel);
}

void MainWindow::setup_shortcuts() {
    // ESC key to exit
    auto* esc_shortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(esc_shortcut, &QShortcut::activated, this, &QMainWindow::close);

    // F1 key to show config
    auto* f1_shortcut = new QShortcut(QKeySequence(Qt::Key_F1), this);
    connect(f1_shortcut, &QShortcut::activated, this, &MainWindow::on_show_config);

    // P key to pause
    auto* p_shortcut = new QShortcut(QKeySequence(Qt::Key_P), this);
    connect(p_shortcut, &QShortcut::activated, this, &MainWindow::on_toggle_pause);
}

void MainWindow::set_game(PongGame* game) {
    m_game = game;
    if (m_stats_overlay) {
        m_stats_overlay->set_game_over(false);
    }
}

void MainWindow::update_ui() {
    if (!m_game || !m_stats_overlay) {
        return;
    }

    // Update statistics overlay
    GameStatistics stats = m_game->get_statistics();
    m_stats_overlay->update_stats(stats);

    // Update score
    GameState state;
    // Note: We need to access game state - for now, we'll update via game
    m_stats_overlay->update_score(0, 0); // Will be updated by game

    // Update game over state
    m_stats_overlay->set_game_over(m_game->is_game_over());
}

void MainWindow::on_game_loop() {
    if (!m_game) {
        return;
    }

    // Calculate delta time
    auto current_time = std::chrono::high_resolution_clock::now();
    float delta_time = std::chrono::duration<float, std::milli>(current_time - m_last_frame_time).count() / 1000.0f;
    m_last_frame_time = current_time;

    // Update game
    m_game->update(delta_time);

    // Update UI
    update_ui();

    // FPS calculation
    m_frame_count++;
    m_fps_frame_count++;

    auto fps_time = std::chrono::duration<float, std::milli>(current_time - m_last_fps_time).count();
    if (fps_time >= 1000.0f) {
        float fps = m_fps_frame_count / (fps_time / 1000.0f);
        m_fps_frame_count = 0;
        m_last_fps_time = current_time;
    }
}

void MainWindow::on_show_config() {
    if (m_game) {
        m_game->show_config_dialog();
    }
}

void MainWindow::on_toggle_pause() {
    if (!m_game) {
        return;
    }

    if (m_game->is_paused()) {
        m_game->resume_game();
    } else {
        m_game->pause_game();
    }
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    // Handle window resize if needed
}

void MainWindow::keyPressEvent(QKeyEvent* event) {
    if (!m_game) {
        QMainWindow::keyPressEvent(event);
        return;
    }

    m_game->handle_key_down(event->key());
    QMainWindow::keyPressEvent(event);
}

void MainWindow::keyReleaseEvent(QKeyEvent* event) {
    if (!m_game) {
        QMainWindow::keyReleaseEvent(event);
        return;
    }

    m_game->handle_key_up(event->key());
    QMainWindow::keyReleaseEvent(event);
}

void MainWindow::mouseMoveEvent(QMouseEvent* event) {
    if (!m_game) {
        QMainWindow::mouseMoveEvent(event);
        return;
    }

    // Convert screen coordinates to game coordinates
    float x = event->position().x() / 40.0f; // Scale to game coordinates
    float y = event->position().y() / 60.0f;

    m_game->handle_mouse_move(x, y);
    QMainWindow::mouseMoveEvent(event);
}

void MainWindow::mousePressEvent(QMouseEvent* event) {
    if (!m_game) {
        QMainWindow::mousePressEvent(event);
        return;
    }

    m_game->handle_mouse_button(event->button(), true);
    QMainWindow::mousePressEvent(event);
}

void MainWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (!m_game) {
        QMainWindow::mouseReleaseEvent(event);
        return;
    }

    m_game->handle_mouse_button(event->button(), false);
    QMainWindow::mouseReleaseEvent(event);
}
#endif

// ============================================================================
// Main function
// ============================================================================

int main(int argc, char* argv[]) {
    // Check for headless mode
    bool headless_mode = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--headless") == 0 || std::strcmp(argv[i], "-h") == 0) {
            headless_mode = true;
            break;
        }
    }

#ifdef WARPLOOM_HAS_QT_VULKAN
    // Initialize Qt6 application only if not in headless mode
    QPointer<QApplication> app = nullptr;
    if (!headless_mode) {
        app = new QApplication(argc, argv);
    }
    std::cout << "=== OmniCpp 3D Pong Game (Vulkan + Qt6) ===" << std::endl;
#else
    std::cout << "=== OmniCpp 3D Pong Game (Vulkan + GLFW) ===" << std::endl;
#endif
    std::cout << "This example demonstrates 3D Vulkan rendering with the OmniCpp engine." << std::endl;
    if (headless_mode) {
        std::cout << "Running in HEADLESS mode (no graphics)" << std::endl;
    }
    std::cout << std::endl;

    // If headless mode, run a simple simulation without any UI
    if (headless_mode) {
        std::cout << "Running headless simulation..." << std::endl;
        
        PongGame pong_game;
        GameConfig game_config;
        game_config.ball_radius = 0.3f;
        game_config.ball_speed = 8.0f;
        game_config.paddle_width = 0.5f;
        game_config.paddle_height = 2.0f;
        game_config.paddle_speed = 10.0f;
        game_config.win_score = 10;
        game_config.enable_ai = true;
        game_config.ai_difficulty = 0.5f;
        
        if (!pong_game.initialize(game_config)) {
            std::cerr << "Failed to initialize pong game!" << std::endl;
            return 1;
        }
        
        std::cout << "Pong game initialized successfully!" << std::endl;
        std::cout << "Running 1000 frames of simulation..." << std::endl;
        
        // Run simulation for 1000 frames
        for (int i = 0; i < 1000; i++) {
            float delta_time = 1.0f / 60.0f;  // 60 FPS
            pong_game.update(delta_time);
            
            if (i % 100 == 0) {
                auto stats = pong_game.get_statistics();
                std::cout << "Frame " << i << ": FPS=" << stats.fps 
                          << ", Ball=(" << pong_game.get_config().ball_radius << ")" << std::endl;
            }
        }
        
        std::cout << "Headless simulation complete!" << std::endl;
        return 0;
    }

    // Detect Wayland platform for logging
    bool is_wayland = false;
#ifdef WARPLOOM_HAS_QT_VULKAN
    const char* qt_platform = std::getenv("QT_QPA_PLATFORM");
    is_wayland = (qt_platform && (std::strcmp(qt_platform, "wayland") == 0));
    if (is_wayland) {
        std::cout << "Wayland platform detected" << std::endl;
    } else {
        std::cout << "Qt Platform: " << (qt_platform ? qt_platform : "default") << std::endl;
    }
#endif

    // Create window manager
    OmniCpp::Engine::Window::WindowConfig window_config;
    window_config.title = "OmniCpp 3D Pong - Vulkan";
    window_config.width = 800;
    window_config.height = 600;
    window_config.fullscreen = false;
    window_config.vsync = true;
    window_config.resizable = true;

    OmniCpp::Engine::Window::WindowManager window_manager;

#ifdef WARPLOOM_HAS_QT_VULKAN
    // Pass the Qt6 application to the window manager
    window_manager.set_qt_application(app.data());

    // Set close callback to handle window close events
    window_manager.set_close_callback([]() {
        std::cout << "Window closed by user" << std::endl;
    });
#endif

    if (!window_manager.initialize(window_config)) {
        std::cerr << "Failed to initialize window manager!" << std::endl;
        return 1;
    }

    std::cout << "Window manager initialized successfully!" << std::endl;

    // Create renderer
    OmniCpp::Engine::Graphics::RendererConfig renderer_config;
    renderer_config.vsync = true;
    renderer_config.msaa_samples = 4;
    renderer_config.enable_debug = true;

    OmniCpp::Engine::Graphics::Renderer renderer;
    renderer.set_window_manager(&window_manager);

    bool renderer_initialized = renderer.initialize(renderer_config);
    if (!renderer_initialized) {
        std::cerr << "Failed to initialize renderer!" << std::endl;
        std::cerr << "Continuing without rendering - window will remain visible for testing" << std::endl;
        std::cerr << std::endl;
    } else {
        std::cout << "Vulkan renderer initialized successfully!" << std::endl;
        std::cout << std::endl;
    }

    // Create pong game
    PongGame pong_game;
    GameConfig game_config;
    game_config.ball_radius = 0.3f;
    game_config.ball_speed = 8.0f;
    game_config.paddle_width = 0.5f;
    game_config.paddle_height = 2.0f;
    game_config.paddle_speed = 10.0f;
    game_config.win_score = 10;
    game_config.enable_ai = true;
    game_config.ai_difficulty = 0.5f;

    if (!pong_game.initialize(game_config)) {
        std::cerr << "Failed to initialize pong game!" << std::endl;
        return 1;
    }

    std::cout << "Pong game initialized successfully!" << std::endl;
    std::cout << "Controls:" << std::endl;
    std::cout << "  W/S - Move left paddle" << std::endl;
    std::cout << "  Up/Down - Move right paddle (or use AI)" << std::endl;
    std::cout << "  ESC - Exit game" << std::endl;
    std::cout << "  P - Pause/Resume game" << std::endl;
    std::cout << "  F1 - Open settings dialog" << std::endl;
    std::cout << std::endl;

#ifdef WARPLOOM_HAS_QT_VULKAN
    // Create Qt6 main window with UI
    QWindow* vulkan_window = window_manager.get_qt_window();
    if (!vulkan_window) {
        std::cerr << "Failed to get Qt window from window manager!" << std::endl;
        return 1;
    }

    MainWindow main_window(vulkan_window);
    main_window.set_game(&pong_game);
    main_window.show();

    // Ensure window is visible on Wayland
    if (is_wayland) {
        std::cout << "Requesting window visibility on Wayland..." << std::endl;
        main_window.raise();
        main_window.activateWindow();
        std::cout << "Window visibility requested" << std::endl;
    }

    std::cout << "Qt6 UI created successfully!" << std::endl;
    std::cout << std::endl;
#endif

    // Simple game loop
    std::cout << "Starting game loop..." << std::endl;
    std::cout << "Press ESC or close the window to exit." << std::endl;
    std::cout << std::endl;

    const float target_fps = 60.0f;
    const float frame_time = 1000.0f / target_fps;
    auto last_frame_time = std::chrono::high_resolution_clock::now();

    // FPS measurement variables
    uint32_t frame_count = 0;
    auto last_fps_time = std::chrono::high_resolution_clock::now();
    float current_fps = 0.0f;

    // Main game loop
    while (!window_manager.should_close()) {
        auto current_time = std::chrono::high_resolution_clock::now();
        float delta_time = std::chrono::duration<float, std::milli>(current_time - last_frame_time).count();

        if (delta_time >= frame_time) {
            last_frame_time = current_time;

            // Update engine
            window_manager.update();

            // Update game
            pong_game.update(delta_time / 1000.0f);

            // Render frame only if renderer is initialized
            if (renderer_initialized) {
                // Update renderer with current game state
                Pong::GameState game_state = pong_game.get_game_state();
                renderer.set_ball_position(game_state.ball_position.x, game_state.ball_position.y);
                renderer.set_paddle_position(true, game_state.paddle_left_y);   // Left paddle
                renderer.set_paddle_position(false, game_state.paddle_right_y); // Right paddle

                renderer.render();

                // Present frame
                renderer.present();

                // Print frame count every 60 frames
                uint32_t renderer_frame_count = renderer.get_frame_count();
                if (renderer_frame_count % 60 == 0) {
                    std::cout << "Frame: " << renderer_frame_count << std::endl;
                }
            }

            // Calculate and log FPS every second
            auto current_fps_time = std::chrono::high_resolution_clock::now();
            auto fps_elapsed = std::chrono::duration<float, std::milli>(current_fps_time - last_fps_time).count();
            if (fps_elapsed >= 1000.0f) {
                current_fps = (frame_count / fps_elapsed) * 1000.0f;
                if (is_wayland) {
                    std::cout << "[Wayland] FPS: " << static_cast<int>(current_fps)
                              << " (frames: " << frame_count << ")" << std::endl;
                } else {
                    std::cout << "FPS: " << static_cast<int>(current_fps)
                              << " (frames: " << frame_count << ")" << std::endl;
                }
                // Reset FPS counter
                frame_count = 0;
                last_fps_time = current_fps_time;
            }
        } else {
            // Sleep to maintain target FPS
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::cout << std::endl;
    std::cout << "Game loop finished." << std::endl;
    if (renderer_initialized) {
        std::cout << "Total frames rendered: " << renderer.get_frame_count() << std::endl;
    } else {
        std::cout << "No frames rendered (renderer not initialized)" << std::endl;
    }

    GameStatistics final_stats = pong_game.get_statistics();
    std::cout << "Total frames: " << final_stats.total_frames << std::endl;
    std::cout << "Total time: " << final_stats.total_time << " seconds" << std::endl;
    std::cout << "Average FPS: " << final_stats.fps << std::endl;

    // Cleanup
    if (renderer_initialized) {
        renderer.shutdown();
    }
    window_manager.shutdown();

#ifdef WARPLOOM_HAS_QT_VULKAN
    // Qt6 cleanup - MainWindow will be deleted automatically
    delete app.data();
#endif

    std::cout << std::endl;
    std::cout << "=== 3D Pong Game Complete ===" << std::endl;

    return 0;
}
