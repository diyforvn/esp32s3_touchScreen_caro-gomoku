/*
   
    - Mode: Offline Only
    - AI: Minimax Logic preserved
    Minimax/Alpha-Beta
*/
#include <Arduino.h>
#include <lvgl.h>
#include "JC3248W535EN_Touch_LCD.h" 
#include "esp_heap_caps.h"
#include <Ticker.h>
#include <vector>
#include <algorithm>

// Font declarations
extern const lv_font_t lv_font_montserrat_16;
extern const lv_font_t lv_font_montserrat_22; 

#define DEBUG_MODE

#ifdef DEBUG_MODE
  #define DEBUG_PRINT(x) Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
  #define DEBUG_PRINTF(format, ...) Serial.printf(format, ##__VA_ARGS__)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
  #define DEBUG_PRINTF(format, ...)
#endif

#define resetPin 9  

/*######################### TFT ###############*/
static const uint16_t screenWidth  = 480;
static const uint16_t screenHeight = 320;

enum { SCREENBUFFER_SIZE_PIXELS = screenWidth * screenHeight}; 
lv_color_t* buf = nullptr;
JC3248W535EN tft;

/*######################### GAME CỜ CARO ###############*/
#define BOARD_SIZE 10 
#define WIN_COUNT 5   

// --- Game Modes & AI Levels ---
enum GameMode { MODE_PVP, MODE_PVE };
enum AILevel { AI_EASY = 1, AI_MEDIUM = 2, AI_HARD = 3 };

static GameMode current_mode = MODE_PVP;
static AILevel current_ai_level = AI_EASY;
static bool is_ai_thinking = false;

// --- Game State ---
static char board[BOARD_SIZE][BOARD_SIZE]; 
static char currentPlayer; // 'X' (Người/Máy 1), 'O' (Người/Máy 2)
static bool game_over;
static bool game_running = false; 
static int move_count = 0; 

// --- UI Objects ---
static lv_obj_t* cell_panels[BOARD_SIZE][BOARD_SIZE];
static lv_obj_t* cells[BOARD_SIZE][BOARD_SIZE]; 
static lv_obj_t* status_label;
static lv_obj_t* label_score_x;
static lv_obj_t* label_score_o;
static lv_obj_t* reset_btn;
static lv_obj_t* reset_score_btn;
static lv_obj_t* menu_btn; 
static lv_obj_t* mode_label; 

// --- Styles ---
static lv_style_t style_cell;
static lv_style_t style_pressed;
static lv_style_t style_x_bg; 
static lv_style_t style_o_bg; 
static lv_style_t style_blink;

// --- Score ---
static int score_x = 0;
static int score_o = 0;

// --- Blink & Win ---
static lv_timer_t* blink_timer = nullptr;
static int win_pos_r[WIN_COUNT];
static int win_pos_c[WIN_COUNT];
static bool win_positions_valid = false;
static bool blink_state = false;

static bool start_with_x = true; 

// --- Multitasking ---
SemaphoreHandle_t lvgl_mutex;
#define LVGL_LOCK()   xSemaphoreTake(lvgl_mutex, portMAX_DELAY)
#define LVGL_UNLOCK() xSemaphoreGive(lvgl_mutex)

// --- AI Structs ---
struct Point {
    int r, c;
};

// --- Prototypes ---
void create_menu_ui();
void create_game_ui();
void reset_game();
void make_move(int r, int c);
void start_ai_task();
static char check_win_and_fill_positions();

/*##################### DISP FLUSH ########################*/
void my_disp_flush (lv_display_t *disp, const lv_area_t *area, uint8_t *pixelmap) {
    uint32_t w = ( area->x2 - area->x1 + 1 );
    uint32_t h = ( area->y2 - area->y1 + 1 );
    if (LV_COLOR_16_SWAP) {
        size_t len = lv_area_get_size( area );
        lv_draw_sw_rgb565_swap( pixelmap, len );
    }
    if (tft.gfx) tft.gfx->draw16bitRGBBitmap( area->x1, area->y1, (uint16_t*)pixelmap, w, h );
    tft.flush();
    lv_disp_flush_ready( disp );
}

void my_touchpad_read (lv_indev_t * indev_driver, lv_indev_data_t * data) {
    uint16_t touchX = 0, touchY = 0;
    bool touched = tft.getTouchPoint(touchX, touchY); 
    if (!touched) {
        data->state = LV_INDEV_STATE_REL;
    } else {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = touchX;
        data->point.y = touchY;      
    }
}

static uint32_t my_tick_get_cb (void) { return millis(); }

bool screenSetup() {
    lv_init();
    if (!tft.begin()) return false;       
    if (tft.gfx) tft.gfx->setRotation(1); 

    static lv_disp_t* disp;
    disp = lv_display_create( screenWidth, screenHeight );
    buf = (lv_color_t*) heap_caps_malloc(SCREENBUFFER_SIZE_PIXELS * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return false;

    lv_display_set_buffers( disp, buf, NULL, SCREENBUFFER_SIZE_PIXELS * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL );
    lv_display_set_flush_cb( disp, my_disp_flush );

    static lv_indev_t* indev;
    indev = lv_indev_create();
    lv_indev_set_type( indev, LV_INDEV_TYPE_POINTER );
    lv_indev_set_read_cb( indev, my_touchpad_read );

    lv_tick_set_cb( my_tick_get_cb );
    tft.clear(0, 0, 0); 
    return true;
}

void lvglTask(void *pvParameters) {
  while (1) {
    if (xSemaphoreTake(lvgl_mutex, portMAX_DELAY)) {
      lv_timer_handler();
      xSemaphoreGive(lvgl_mutex);
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// =================================================================
// =========================== AI LOGIC ============================
// =================================================================

// Điểm số đánh giá
#define SCORE_WIN       1000000
#define SCORE_OPEN_4    50000   
#define SCORE_BLOCKED_4 10000   
#define SCORE_OPEN_3    5000    
#define SCORE_BLOCKED_3 1000
#define SCORE_OPEN_2    500

std::vector<Point> get_neighbor_moves(int range) {
    std::vector<Point> moves;
    bool visited[BOARD_SIZE][BOARD_SIZE] = {false};

    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            if (board[r][c] != ' ') {
                for (int dr = -range; dr <= range; dr++) {
                    for (int dc = -range; dc <= range; dc++) {
                        int nr = r + dr;
                        int nc = c + dc;
                        if (nr >= 0 && nr < BOARD_SIZE && nc >= 0 && nc < BOARD_SIZE) {
                            if (board[nr][nc] == ' ' && !visited[nr][nc]) {
                                moves.push_back({nr, nc});
                                visited[nr][nc] = true;
                            }
                        }
                    }
                }
            }
        }
    }
    
    if (moves.empty()) {
        moves.push_back({BOARD_SIZE/2, BOARD_SIZE/2});
    }
    return moves;
}

int evaluate_line(int count, int blocked, char player) {
    if (count >= 5) return SCORE_WIN;
    
    int score = 0;
    if (count == 4) score = (blocked == 0) ? SCORE_OPEN_4 : (blocked == 1 ? SCORE_BLOCKED_4 : 0);
    else if (count == 3) score = (blocked == 0) ? SCORE_OPEN_3 : (blocked == 1 ? SCORE_BLOCKED_3 : 0);
    else if (count == 2) score = (blocked == 0) ? SCORE_OPEN_2 : 0;

    return (player == 'O') ? score : -score; 
}

int evaluate_board_gomoku() {
    int total_score = 0;
    int dr[] = {0, 1, 1, 1};
    int dc[] = {1, 0, 1, -1};

    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            if (board[r][c] == ' ') continue;
            
            char p = board[r][c];
            
            for (int dir = 0; dir < 4; dir++) {
                int prev_r = r - dr[dir];
                int prev_c = c - dc[dir];
                if (prev_r >= 0 && prev_r < BOARD_SIZE && prev_c >= 0 && prev_c < BOARD_SIZE && board[prev_r][prev_c] == p) {
                    continue;
                }

                int count = 0;
                int blocked = 0;
                
                if (prev_r < 0 || prev_r >= BOARD_SIZE || prev_c < 0 || prev_c >= BOARD_SIZE || board[prev_r][prev_c] != ' ') {
                    blocked++;
                }

                int cur_r = r;
                int cur_c = c;
                while (cur_r >= 0 && cur_r < BOARD_SIZE && cur_c >= 0 && cur_c < BOARD_SIZE && board[cur_r][cur_c] == p) {
                    count++;
                    cur_r += dr[dir];
                    cur_c += dc[dir];
                }

                if (cur_r < 0 || cur_r >= BOARD_SIZE || cur_c < 0 || cur_c >= BOARD_SIZE || board[cur_r][cur_c] != ' ') {
                    blocked++;
                }
                
                total_score += evaluate_line(count, blocked, p);
            }
        }
    }
    return total_score;
}

int minimax(int depth, int alpha, int beta, bool isMaximizing) {
    int score = evaluate_board_gomoku();
    if (abs(score) > SCORE_WIN / 2) return score; 
    if (depth == 0) return score;

    std::vector<Point> moves = get_neighbor_moves(1); 
    if (moves.empty()) return 0;

    if (isMaximizing) { // AI ('O')
        int maxEval = -2000000000;
        for (auto m : moves) {
            board[m.r][m.c] = 'O';
            int eval = minimax(depth - 1, alpha, beta, false);
            board[m.r][m.c] = ' '; // Undo
            maxEval = std::max(maxEval, eval);
            alpha = std::max(alpha, eval);
            if (beta <= alpha) break;
        }
        return maxEval;
    } else { // Human ('X')
        int minEval = 2000000000;
        for (auto m : moves) {
            board[m.r][m.c] = 'X';
            int eval = minimax(depth - 1, alpha, beta, true);
            board[m.r][m.c] = ' '; // Undo
            minEval = std::min(minEval, eval);
            beta = std::min(beta, eval);
            if (beta <= alpha) break;
        }
        return minEval;
    }
}

void ai_play_task(void *parameter) {
    if (!game_running) {
        is_ai_thinking = false;
        vTaskDelete(NULL);
        return;
    }

    LVGL_LOCK();
    lv_label_set_text(status_label, "AI Thinking...");
    LVGL_UNLOCK();

    Point bestMove = {-1, -1};

    if (current_ai_level == AI_EASY) {
        std::vector<Point> moves = get_neighbor_moves(1);
        if (!moves.empty()) {
            int idx = rand() % moves.size();
            bestMove = moves[idx];
        }
    } else {
        int depth = (current_ai_level == AI_MEDIUM) ? 1 : 2; 
        std::vector<Point> moves = get_neighbor_moves(1);
        int bestVal = -2000000000;

        for (auto m : moves) {
            if (!game_running) break;

            board[m.r][m.c] = 'O'; 
            int moveVal = minimax(depth - 1, -2000000000, 2000000000, false); 
            board[m.r][m.c] = ' '; 

            if (moveVal > bestVal) {
                bestMove = m;
                bestVal = moveVal;
            }
            vTaskDelay(1); 
        }
    }

    is_ai_thinking = false;

    if (game_running && bestMove.r != -1) {
        LVGL_LOCK();
        make_move(bestMove.r, bestMove.c);
        LVGL_UNLOCK();
    } else if (bestMove.r == -1) {
        LVGL_LOCK();
        lv_label_set_text(status_label, "Draw!");
        LVGL_UNLOCK();
    }

    vTaskDelete(NULL);
}

void start_ai_task() {
    is_ai_thinking = true;
    xTaskCreate(ai_play_task, "AI_Gomoku", 16000, NULL, 1, NULL);
}

// =================================================================
// =========================== GAME LOGIC ==========================
// =================================================================

static char check_win_and_fill_positions() {
    int empty_cells = 0;
    
    for (int i = 0; i < BOARD_SIZE; i++) {
        for (int j = 0; j < BOARD_SIZE; j++) {
            char current_player = board[i][j];
            if (current_player == ' ') {
                empty_cells++;
                continue;
            }

            // 1. Horizontal
            if (j <= BOARD_SIZE - WIN_COUNT) {
                int count = 0;
                for (int k = 0; k < WIN_COUNT; k++) {
                    if (board[i][j + k] == current_player) count++;
                    else break;
                }
                if (count == WIN_COUNT) {
                    for (int k = 0; k < WIN_COUNT; k++) { win_pos_r[k] = i; win_pos_c[k] = j + k; }
                    win_positions_valid = true;
                    return current_player;
                }
            }

            // 2. Vertical
            if (i <= BOARD_SIZE - WIN_COUNT) {
                int count = 0;
                for (int k = 0; k < WIN_COUNT; k++) {
                    if (board[i + k][j] == current_player) count++;
                    else break;
                }
                if (count == WIN_COUNT) {
                    for (int k = 0; k < WIN_COUNT; k++) { win_pos_r[k] = i + k; win_pos_c[k] = j; }
                    win_positions_valid = true;
                    return current_player;
                }
            }

            // 3. Diagonal "\"
            if (i <= BOARD_SIZE - WIN_COUNT && j <= BOARD_SIZE - WIN_COUNT) {
                int count = 0;
                for (int k = 0; k < WIN_COUNT; k++) {
                    if (board[i + k][j + k] == current_player) count++;
                    else break;
                }
                if (count == WIN_COUNT) {
                    for (int k = 0; k < WIN_COUNT; k++) { win_pos_r[k] = i + k; win_pos_c[k] = j + k; }
                    win_positions_valid = true;
                    return current_player;
                }
            }

            // 4. Diagonal "/"
            if (i <= BOARD_SIZE - WIN_COUNT && j >= WIN_COUNT - 1) {
                int count = 0;
                for (int k = 0; k < WIN_COUNT; k++) {
                    if (board[i + k][j - k] == current_player) count++;
                    else break;
                }
                if (count == WIN_COUNT) {
                    for (int k = 0; k < WIN_COUNT; k++) { win_pos_r[k] = i + k; win_pos_c[k] = j - k; }
                    win_positions_valid = true;
                    return current_player;
                }
            }
        }
    }

    win_positions_valid = false;
    return (empty_cells == 0) ? 'D' : ' ';
}

static void stop_blinking() {
    if (blink_timer) {
        lv_timer_del(blink_timer);
        blink_timer = nullptr;
    }
    blink_state = false;
    
    if (win_positions_valid) {
        for (int k = 0; k < WIN_COUNT; k++) {
            int r = win_pos_r[k], c = win_pos_c[k];
            if (board[r][c] == 'X') {
                lv_obj_remove_style(cell_panels[r][c], &style_blink, 0);
                lv_obj_add_style(cell_panels[r][c], &style_x_bg, 0);
            } else if (board[r][c] == 'O') {
                lv_obj_remove_style(cell_panels[r][c], &style_blink, 0);
                lv_obj_add_style(cell_panels[r][c], &style_o_bg, 0);
            }
        }
    }
}

static void blink_timer_cb(lv_timer_t* t) {
    (void)t;
    blink_state = !blink_state;

    for (int k = 0; k < WIN_COUNT; k++) {
        int r = win_pos_r[k], c = win_pos_c[k];
        if (!cell_panels[r][c]) continue;
        if (blink_state) {
            lv_obj_add_style(cell_panels[r][c], &style_blink, 0);
        } else {
            lv_obj_remove_style(cell_panels[r][c], &style_blink, 0);
        }
    }
}

static void update_score_labels() {
    char buf[32];
    sprintf(buf, "X: %d", score_x);
    lv_label_set_text(label_score_x, buf);

    sprintf(buf, "O: %d", score_o);
    lv_label_set_text(label_score_o, buf);
}

void reset_game() {
    stop_blinking();
    win_positions_valid = false;
    game_over = false;
    game_running = true; 
    move_count = 0;

    start_with_x = !start_with_x;
    currentPlayer = start_with_x ? 'X' : 'O';

    for (int i = 0; i < BOARD_SIZE; i++) {
        for (int j = 0; j < BOARD_SIZE; j++) {
            board[i][j] = ' ';
            if (cell_panels[i][j]) {
                lv_obj_remove_style(cell_panels[i][j], &style_x_bg, 0);
                lv_obj_remove_style(cell_panels[i][j], &style_o_bg, 0);
                lv_obj_remove_style(cell_panels[i][j], &style_blink, 0);
                if(cells[i][j]) lv_label_set_text(cells[i][j], "");
            }
        }
    }

    if (status_label) {
        char buf[32];
        sprintf(buf, "%c Turn", currentPlayer);
        lv_label_set_text(status_label, buf);
    }
    
    if (mode_label) {
        if (current_mode == MODE_PVP) {
            lv_label_set_text(mode_label, "Mode: PvP");
        } else {
            const char* lvl = (current_ai_level == AI_EASY) ? "Easy" : 
                              (current_ai_level == AI_MEDIUM) ? "Medium" : "Hard";
            lv_label_set_text_fmt(mode_label, "PvE (%s)", lvl);
        }
    }

    if (current_mode == MODE_PVE && currentPlayer == 'O') {
        start_ai_task();
    }
}

void make_move(int r, int c) {
    board[r][c] = currentPlayer;
    move_count++;

    if (currentPlayer == 'X') {
        lv_obj_remove_style(cell_panels[r][c], &style_o_bg, 0);
        lv_obj_add_style(cell_panels[r][c], &style_x_bg, 0);
    } else {
        lv_obj_remove_style(cell_panels[r][c], &style_x_bg, 0);
        lv_obj_add_style(cell_panels[r][c], &style_o_bg, 0);
    }

    char winner = check_win_and_fill_positions();
    if (winner != ' ') {
        game_over = true;
        if (winner == 'X') score_x++;
        if (winner == 'O') score_o++;
        update_score_labels();

        if (winner == 'D') {
            lv_label_set_text(status_label, "Draw!");
        } else {
            char buf[20];
            sprintf(buf, "%c WIN!", winner);
            lv_label_set_text(status_label, buf);
            if (win_positions_valid) {
                if (blink_timer) lv_timer_del(blink_timer);
                blink_state = false;
                blink_timer = lv_timer_create(blink_timer_cb, 300, NULL); 
            }
        }
    } else {
        currentPlayer = (currentPlayer == 'X') ? 'O' : 'X';
        char buf[20];
        sprintf(buf, "%c Turn", currentPlayer);
        lv_label_set_text(status_label, buf);

        if (current_mode == MODE_PVE && currentPlayer == 'O' && !game_over) {
            start_ai_task();
        }
    }
}

static void game_cell_cb(lv_event_t * e) {
    if (game_over) return;
    
    if (current_mode == MODE_PVE && currentPlayer == 'O') return;
    if (is_ai_thinking) return;

    uintptr_t index = (uintptr_t)lv_event_get_user_data(e);
    int row = index / BOARD_SIZE;
    int col = index % BOARD_SIZE;

    if (board[row][col] == ' ') {
        make_move(row, col);
    }
}

// =================================================================
// =========================== UI CREATION =========================
// =================================================================

void init_styles() {
    lv_style_init(&style_cell);
    lv_style_set_radius(&style_cell, 5); 
    lv_style_set_border_width(&style_cell, 2); 
    lv_style_set_border_color(&style_cell, lv_palette_main(LV_PALETTE_GREY));
    lv_style_set_bg_color(&style_cell, lv_palette_lighten(LV_PALETTE_GREY, 4));
    lv_style_set_bg_opa(&style_cell, LV_OPA_50);

    lv_style_init(&style_pressed);
    lv_style_set_bg_color(&style_pressed, lv_palette_main(LV_PALETTE_YELLOW));

    lv_style_init(&style_x_bg);
    lv_style_set_bg_color(&style_x_bg, lv_palette_main(LV_PALETTE_BLUE));
    lv_style_set_bg_opa(&style_x_bg, LV_OPA_COVER);
    lv_style_set_radius(&style_x_bg, 5);

    lv_style_init(&style_o_bg);
    lv_style_set_bg_color(&style_o_bg, lv_palette_main(LV_PALETTE_RED));
    lv_style_set_bg_opa(&style_o_bg, LV_OPA_COVER);
    lv_style_set_radius(&style_o_bg, 5);

    lv_style_init(&style_blink);
    lv_style_set_border_color(&style_blink, lv_palette_main(LV_PALETTE_YELLOW));
    lv_style_set_border_width(&style_blink, 4);
    lv_style_set_bg_opa(&style_blink, LV_OPA_TRANSP); 
}

// --- MENU UI ---
void create_menu_ui() {
    lv_obj_t * scr = lv_screen_active(); 
    lv_obj_clean(scr);
    lv_obj_set_layout(scr, 0); 
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x202020), 0);

    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "GOMOKU (CARO) ESP32");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    static lv_style_t style_btn;
    lv_style_init(&style_btn);
    lv_style_set_bg_color(&style_btn, lv_palette_main(LV_PALETTE_BLUE));
    lv_style_set_width(&style_btn, 200);
    lv_style_set_height(&style_btn, 50);
    lv_style_set_radius(&style_btn, 10);

    auto create_btn = [&](const char* txt, int y_ofs, lv_color_t color, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_button_create(scr);
        lv_obj_add_style(btn, &style_btn, 0);
        lv_obj_set_style_bg_color(btn, color, 0);
        lv_obj_align(btn, LV_ALIGN_CENTER, 0, y_ofs);
        
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, txt);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    };

    create_btn("Player vs Player", -60, lv_palette_main(LV_PALETTE_BLUE), [](lv_event_t* e){
        current_mode = MODE_PVP;
        create_game_ui();
    });

    create_btn("PvE - Easy", 0, lv_palette_main(LV_PALETTE_GREEN), [](lv_event_t* e){
        current_mode = MODE_PVE;
        current_ai_level = AI_EASY;
        create_game_ui();
    });

    create_btn("PvE - Medium", 60, lv_palette_main(LV_PALETTE_ORANGE), [](lv_event_t* e){
        current_mode = MODE_PVE;
        current_ai_level = AI_MEDIUM;
        create_game_ui();
    });

    create_btn("PvE - Hard", 120, lv_palette_main(LV_PALETTE_RED), [](lv_event_t* e){
        current_mode = MODE_PVE;
        current_ai_level = AI_HARD;
        create_game_ui();
    });
}

// --- GAME UI ---
void create_game_ui() {
    lv_obj_t * scr = lv_screen_active(); 
    lv_obj_clean(scr); 
    init_styles();

    const lv_coord_t cell_size = 28; 
    const lv_coord_t padding = 2;   
    const lv_coord_t grid_size = cell_size * BOARD_SIZE + padding * (BOARD_SIZE - 1); 

    lv_obj_set_layout(scr, LV_LAYOUT_GRID);
    static lv_coord_t main_col_dsc[] = {80, grid_size, 80, LV_GRID_TEMPLATE_LAST};
    static lv_coord_t main_row_dsc[] = {LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST}; 

    lv_obj_set_style_grid_column_dsc_array(scr, main_col_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(scr, main_row_dsc, 0);
    
    lv_obj_t* left_panel = lv_obj_create(scr);
    lv_obj_set_grid_cell(left_panel, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_layout(left_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(left_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(left_panel, 5, 0); 
    lv_obj_set_style_border_width(left_panel, 0, 0);

    menu_btn = lv_button_create(left_panel);
    lv_obj_set_width(menu_btn, 70);
    lv_obj_set_style_bg_color(menu_btn, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_t* m_lbl = lv_label_create(menu_btn);
    lv_label_set_text(m_lbl, "Menu");
    lv_obj_center(m_lbl);
    lv_obj_add_event_cb(menu_btn, [](lv_event_t* e){
        game_running = false;
        stop_blinking();
        create_menu_ui();
    }, LV_EVENT_CLICKED, NULL);

    reset_btn = lv_button_create(left_panel);
    lv_obj_set_width(reset_btn, 70);
    lv_obj_set_style_margin_top(reset_btn, 20, 0);
    lv_obj_t* btn_label = lv_label_create(reset_btn);
    lv_label_set_text(btn_label, "RePlay");
    lv_obj_center(btn_label);
    lv_obj_add_event_cb(reset_btn, [](lv_event_t* e){ reset_game(); }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* grid_cont = lv_obj_create(scr);
    lv_obj_set_size(grid_cont, grid_size, grid_size);
    lv_obj_set_grid_cell(grid_cont, LV_GRID_ALIGN_CENTER, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    lv_obj_set_layout(grid_cont, LV_LAYOUT_GRID); 
    lv_obj_set_style_pad_all(grid_cont, 0, 0); 
    lv_obj_set_style_border_width(grid_cont, 0, 0);
    
    lv_obj_t* right_panel = lv_obj_create(scr);
    lv_obj_set_grid_cell(right_panel, LV_GRID_ALIGN_STRETCH, 2, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_layout(right_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(right_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(right_panel, 5, 0); 
    lv_obj_set_style_border_width(right_panel, 0, 0);

    status_label = lv_label_create(right_panel);
    lv_label_set_text(status_label, "Loading...");
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);

    mode_label = lv_label_create(right_panel);
    lv_label_set_text(mode_label, "");
    lv_obj_set_style_text_font(mode_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_margin_top(mode_label, 10, 0);

    label_score_x = lv_label_create(right_panel);
    lv_label_set_text(label_score_x, "X: 0");
    lv_obj_set_style_text_font(label_score_x, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label_score_x, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_margin_top(label_score_x, 10, 0);

    label_score_o = lv_label_create(right_panel);
    lv_label_set_text(label_score_o, "O: 0");
    lv_obj_set_style_text_font(label_score_o, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label_score_o, lv_palette_main(LV_PALETTE_RED), 0);

    reset_score_btn = lv_button_create(right_panel);
    lv_obj_set_width(reset_score_btn, 70);
    lv_obj_set_style_margin_top(reset_score_btn, 20, 0);
    lv_obj_t* rsl = lv_label_create(reset_score_btn);
    lv_label_set_text(rsl, "Reset");
    lv_obj_center(rsl);
    lv_obj_add_event_cb(reset_score_btn, [](lv_event_t* e){
        score_x = 0; score_o = 0;
        update_score_labels();
    }, LV_EVENT_CLICKED, NULL);

    static lv_coord_t dsc_array[BOARD_SIZE + 1];
    for (int i = 0; i < BOARD_SIZE; i++) dsc_array[i] = cell_size;
    dsc_array[BOARD_SIZE] = LV_GRID_TEMPLATE_LAST;

    lv_obj_set_style_grid_column_dsc_array(grid_cont, dsc_array, 0);
    lv_obj_set_style_grid_row_dsc_array(grid_cont, dsc_array, 0);
    lv_obj_set_style_pad_row(grid_cont, padding, 0); 
    lv_obj_set_style_pad_column(grid_cont, padding, 0); 
    
    for (int i = 0; i < BOARD_SIZE; i++) {
        for (int j = 0; j < BOARD_SIZE; j++) {
            lv_obj_t* cell_panel = lv_obj_create(grid_cont);
            lv_obj_set_grid_cell(cell_panel, LV_GRID_ALIGN_STRETCH, j, 1, LV_GRID_ALIGN_STRETCH, i, 1);
            lv_obj_add_style(cell_panel, &style_cell, 0); 
            lv_obj_add_style(cell_panel, &style_pressed, LV_STATE_PRESSED); 
            
            uintptr_t index = i * BOARD_SIZE + j; 
            lv_obj_add_event_cb(cell_panel, game_cell_cb, LV_EVENT_CLICKED, (void*)index);

            cell_panels[i][j] = cell_panel;
            cells[i][j] = lv_label_create(cell_panel);
            lv_label_set_text(cells[i][j], "");
            lv_obj_center(cells[i][j]); 
        }
    }
    
    reset_game(); 
}


void setup ()
{
    Serial.begin( 115200 ); 
    pinMode(resetPin, INPUT_PULLUP);
    delay(1000);    
    
    if (!screenSetup()) {      
        DEBUG_PRINTLN("LCD Init Failed!");
        while (1);
    }
    
    lvgl_mutex = xSemaphoreCreateMutex();
    
    create_menu_ui(); 

    xTaskCreatePinnedToCore(lvglTask, "LVGL Task", 8192, NULL, 1, NULL, 0);
}

void loop ()
{
    yield();
}