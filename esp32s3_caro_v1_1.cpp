/*
    Version 1.1
    - Đổi màu X,O thành xanh đỏ
    - Hiển thị điểm số X và O
    - Thêm nút reset score
*/
#include "FS.h" 
#include <lvgl.h>
#include "JC3248W535EN_Touch_LCD.h" 
#include "esp_heap_caps.h"
#include <Ticker.h>
#include <Preferences.h>
#include <LittleFS.h>

extern "C" {
  #include "esp_wifi.h"
}


extern const lv_font_t lv_font_montserrat_16;

#define DEBUG_MODE  // Bỏ dòng này đi nếu không muốn bật debug

#ifdef DEBUG_MODE //
  #define DEBUG_PRINT(x) Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
#endif

#define resetPin 9  

/*######################### TFT ###############*/
static const uint16_t screenWidth  = 480;
static const uint16_t screenHeight = 320;

enum { SCREENBUFFER_SIZE_PIXELS = screenWidth * screenHeight}; 
lv_color_t* buf = nullptr;
JC3248W535EN tft; /* JC3248W535EN instance */

/*################## END TFT ####################*/

/*######################### GAME CỜ CARO ###############*/
#define BOARD_SIZE 10 // Kích thước của bàn cờ
#define WIN_COUNT 5   // Số ô liên tiếp để thắng

static char board[BOARD_SIZE][BOARD_SIZE]; // Mảng 2D lưu trạng thái bàn cờ (' ', 'X', 'O')
static char currentPlayer;
static bool game_over;

// lưu panel (ô) chứ không phải label — để đổi màu nền
static lv_obj_t* cell_panels[BOARD_SIZE][BOARD_SIZE];
static lv_obj_t* cells[BOARD_SIZE][BOARD_SIZE]; 
// UI objects
static lv_obj_t* status_label;
static lv_obj_t* reset_btn;

// styles
static lv_style_t style_cell;
static lv_style_t style_text_x;
static lv_style_t style_text_o;
static lv_style_t style_pressed;
static lv_style_t style_x_bg; // nền X
static lv_style_t style_o_bg; // nền O
static lv_style_t style_blink; // style dùng để nhấp nháy (thêm viền hoặc đổi opacity)

// score
static int score_x = 0;
static int score_o = 0;

static lv_obj_t* label_score_x;
static lv_obj_t* label_score_o;
static lv_obj_t* reset_score_btn;

/* blinking related */
static lv_timer_t* blink_timer = nullptr;
static int win_pos_r[WIN_COUNT];
static int win_pos_c[WIN_COUNT];
static bool win_positions_valid = false;
static bool blink_state = false;

/* starting player toggle */
static bool start_with_x = true; // ván đầu X trước; sẽ toggle khi replay

/* LVGL config*/
SemaphoreHandle_t lvgl_mutex;
#define LVGL_LOCK()   xSemaphoreTake(lvgl_mutex, portMAX_DELAY)
#define LVGL_UNLOCK() xSemaphoreGive(lvgl_mutex)

/*##################### END GLVL ########################*/
/* Display flushing */
void my_disp_flush (lv_display_t *disp, const lv_area_t *area, uint8_t *pixelmap)
{
    uint32_t w = ( area->x2 - area->x1 + 1 );
    uint32_t h = ( area->y2 - area->y1 + 1 );

    if (LV_COLOR_16_SWAP) {
        size_t len = lv_area_get_size( area );
        lv_draw_sw_rgb565_swap( pixelmap, len );
    }
    if (tft.gfx) { // Check if gfx is initialized
        tft.gfx->draw16bitRGBBitmap( area->x1, area->y1, (uint16_t*)pixelmap, w, h );
    }
    tft.flush();

    lv_disp_flush_ready( disp );
}


void my_touchpad_read (lv_indev_t * indev_driver, lv_indev_data_t * data)
{
    uint16_t touchX = 0, touchY = 0;

    bool touched = tft.getTouchPoint(touchX, touchY); 

    if (!touched)
    {
        data->state = LV_INDEV_STATE_REL;
    }
    else
    {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = touchX;
        data->point.y = touchY;      
    }
}

/*Set tick routine needed for LVGL internal timings*/
static uint32_t my_tick_get_cb (void) { return millis(); }
bool screenSetup()
{
    lv_init();
    if (!tft.begin())  return false;       
      
    if (tft.gfx) {
        tft.gfx->setRotation(1); // Landscape orientation (480 wide, 320 high)
                                 // Adjust if your panel's rotation 1 is different.
    }

    static lv_disp_t* disp;
    disp = lv_display_create( screenWidth, screenHeight );
    buf = (lv_color_t*) heap_caps_malloc(SCREENBUFFER_SIZE_PIXELS * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!buf) {
          DEBUG_PRINTLN("Failed to allocate LVGL buffer in PSRAM!");
          return false;
      }

    lv_display_set_buffers( disp, buf, NULL, SCREENBUFFER_SIZE_PIXELS * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL );
    lv_display_set_flush_cb( disp, my_disp_flush );

    static lv_indev_t* indev;
    indev = lv_indev_create();
    lv_indev_set_type( indev, LV_INDEV_TYPE_POINTER );
    lv_indev_set_read_cb( indev, my_touchpad_read );

    lv_tick_set_cb( my_tick_get_cb );
    tft.clear(0, 0, 0); // Black color
    return true;
}

void lvglTask(void *pvParameters) {
  while (1) {
    if (xSemaphoreTake(lvgl_mutex, portMAX_DELAY)) {
      lv_timer_handler();
      xSemaphoreGive(lvgl_mutex);
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

/**
 * Tìm dãy thắng (nếu có). Nếu tìm thấy, lưu vị trí vào win_pos_r/win_pos_c và trả về 'X'/'O'.
 * Nếu hòa trả về 'D', nếu chưa kết trả về ' '.
 */
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

    // Hòa
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

static void reset_score_cb(lv_event_t* e) {
    score_x = 0;
    score_o = 0;
    update_score_labels();
}


/**
 * @brief Đặt lại (reset) trò chơi về trạng thái ban đầu
 */
static void reset_game() {
    // stop any blinking
    stop_blinking();
    win_positions_valid = false;

    game_over = false;
    // đổi người đi trước so với ván trước
    start_with_x = !start_with_x;
    currentPlayer = start_with_x ? 'X' : 'O';

    // Xóa bàn cờ logic và bàn cờ UI
    for (int i = 0; i < BOARD_SIZE; i++) {
        for (int j = 0; j < BOARD_SIZE; j++) {
            board[i][j] = ' ';
            if (cells[i][j]) {
                lv_label_set_text(cells[i][j], ""); // giữ rỗng nếu có label
            }
            if (cell_panels[i][j]) {
                lv_obj_remove_style(cell_panels[i][j], &style_x_bg, 0);
                lv_obj_remove_style(cell_panels[i][j], &style_o_bg, 0);
                lv_obj_remove_style(cell_panels[i][j], &style_blink, 0);
            }
        }
    }
    // Cập nhật nhãn trạng thái
    if (status_label) {
        char buf[20];
        sprintf(buf, "%c Turn", currentPlayer);
        lv_label_set_text(status_label, buf);
    }
    
    if(reset_btn) {
        lv_obj_clear_state(reset_btn, LV_STATE_DISABLED);
    }
}

/* Forward Declarations */
static void reset_button_cb(lv_event_t * e);
static void game_cell_cb(lv_event_t * e);

/**
 * @brief Khởi tạo toàn bộ giao diện (UI) của game cờ caro
 */
void create_tic_tac_toe_ui() {
    lv_obj_t * scr = lv_screen_active(); // Lấy màn hình hiện tại
    lv_obj_clean(scr); // Xóa mọi thứ trên màn hình 
    
    // --- 1. Khởi tạo các Style (và Hiệu ứng nhấn) ---
    
    // Style cho ô cờ (viền, bo góc) 
    lv_style_init(&style_cell);
    lv_style_set_radius(&style_cell, 5); 
    lv_style_set_border_width(&style_cell, 2); 
    lv_style_set_border_color(&style_cell, lv_palette_main(LV_PALETTE_GREY));
    lv_style_set_bg_color(&style_cell, lv_palette_lighten(LV_PALETTE_GREY, 4));
    lv_style_set_bg_opa(&style_cell, LV_OPA_50);

    // Style cho trạng thái NHẤN (Chỉ thay đổi màu nền)
    lv_style_init(&style_pressed);
    lv_style_set_bg_color(&style_pressed, lv_palette_main(LV_PALETTE_YELLOW));

    // Style cho chữ X (màu xanh, font nhỏ hơn)
    lv_style_init(&style_text_x);
    lv_style_set_text_color(&style_text_x, lv_palette_main(LV_PALETTE_BLUE));
    lv_style_set_text_font(&style_text_x, &lv_font_montserrat_16); 

    // Style cho chữ O (màu đỏ, font nhỏ hơn)
    lv_style_init(&style_text_o);
    lv_style_set_text_color(&style_text_o, lv_palette_main(LV_PALETTE_RED));
    lv_style_set_text_font(&style_text_o, &lv_font_montserrat_16);

    // Style nền X (blue) - full cover
    lv_style_init(&style_x_bg);
    lv_style_set_bg_color(&style_x_bg, lv_palette_main(LV_PALETTE_BLUE));
    lv_style_set_bg_opa(&style_x_bg, LV_OPA_COVER);
    lv_style_set_radius(&style_x_bg, 5);

    // Style nền O (red) - full cover
    lv_style_init(&style_o_bg);
    lv_style_set_bg_color(&style_o_bg, lv_palette_main(LV_PALETTE_RED));
    lv_style_set_bg_opa(&style_o_bg, LV_OPA_COVER);
    lv_style_set_radius(&style_o_bg, 5);

    // Style blink (ví dụ: tăng độ sáng viền)
    lv_style_init(&style_blink);
    lv_style_set_border_color(&style_blink, lv_palette_main(LV_PALETTE_YELLOW));
    lv_style_set_border_width(&style_blink, 4);
    lv_style_set_bg_opa(&style_blink, LV_OPA_TRANSP); // giữ nền, chỉ viền to hơn

    // --- 2. Cấu hình Bố cục Màn hình (3 Cột) ---
    
    const lv_coord_t cell_size = 28; // Kích thước mỗi ô (28x28)
    const lv_coord_t padding = 2;    // Khoảng cách nhỏ giữa các ô
    const lv_coord_t grid_size = cell_size * BOARD_SIZE + padding * (BOARD_SIZE - 1); // 10x10 = 298px

    lv_obj_set_layout(scr, LV_LAYOUT_GRID);
    static lv_coord_t main_col_dsc[] = {80, grid_size, 80, LV_GRID_TEMPLATE_LAST};
    static lv_coord_t main_row_dsc[] = {LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST}; 

    lv_obj_set_style_grid_column_dsc_array(scr, main_col_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(scr, main_row_dsc, 0);
    
    // LEFT PANEL
    lv_obj_t* left_panel = lv_obj_create(scr);
    lv_obj_set_grid_cell(left_panel, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_layout(left_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(left_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(left_panel, 0, 0); 
    lv_obj_set_style_border_width(left_panel, 0, 0);
    lv_obj_set_style_radius(left_panel, 0, 0);

    // GRID CONTAINER
    lv_obj_t* grid_cont = lv_obj_create(scr);
    lv_obj_set_size(grid_cont, grid_size, grid_size);
    lv_obj_set_grid_cell(grid_cont, LV_GRID_ALIGN_CENTER, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    lv_obj_set_layout(grid_cont, LV_LAYOUT_GRID); 
    lv_obj_set_style_pad_all(grid_cont, 0, 0); 
    lv_obj_set_style_border_width(grid_cont, 0, 0);
    lv_obj_set_style_radius(grid_cont, 0, 0);
    
    // RIGHT PANEL
    lv_obj_t* right_panel = lv_obj_create(scr);
    lv_obj_set_grid_cell(right_panel, LV_GRID_ALIGN_STRETCH, 2, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_layout(right_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(right_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(right_panel, 0, 0); 
    lv_obj_set_style_border_width(right_panel, 0, 0);
    lv_obj_set_style_radius(right_panel, 0, 0);



    // Nút "Chơi Lại"
    reset_btn = lv_button_create(left_panel);
    lv_obj_add_event_cb(reset_btn, reset_button_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* btn_label = lv_label_create(reset_btn);
    lv_label_set_text(btn_label, "RePlay");
    lv_obj_center(btn_label);

    // Nhãn trạng thái
    status_label = lv_label_create(right_panel);
    lv_label_set_text(status_label, "Loading...");
    lv_obj_set_width(status_label, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);

    // SCORE LABELS
    label_score_x = lv_label_create(right_panel);
    lv_label_set_text(label_score_x, "X: 0");
    lv_obj_set_style_text_font(label_score_x, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label_score_x, lv_palette_main(LV_PALETTE_BLUE), 0);

    label_score_o = lv_label_create(right_panel);
    lv_label_set_text(label_score_o, "O: 0");
    lv_obj_set_style_text_font(label_score_o, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label_score_o, lv_palette_main(LV_PALETTE_RED), 0);

    // RESET SCORE BUTTON
    reset_score_btn = lv_button_create(right_panel);
    lv_obj_add_event_cb(reset_score_btn, reset_score_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* rsl = lv_label_create(reset_score_btn);
    lv_label_set_text(rsl, "Reset");
    lv_obj_center(rsl);


    // 5. Cấu hình lưới bàn cờ
    static lv_coord_t dsc_array[BOARD_SIZE + 1];
    for (int i = 0; i < BOARD_SIZE; i++) {
        dsc_array[i] = cell_size;
    }
    dsc_array[BOARD_SIZE] = LV_GRID_TEMPLATE_LAST;

    lv_obj_set_style_grid_column_dsc_array(grid_cont, dsc_array, 0);
    lv_obj_set_style_grid_row_dsc_array(grid_cont, dsc_array, 0);
    lv_obj_set_style_pad_row(grid_cont, padding, 0); 
    lv_obj_set_style_pad_column(grid_cont, padding, 0); 
    
    // 6. Tạo 10x10 ô cờ 
    for (int i = 0; i < BOARD_SIZE; i++) {
        for (int j = 0; j < BOARD_SIZE; j++) {
            lv_obj_t* cell_panel = lv_obj_create(grid_cont);
            lv_obj_set_grid_cell(cell_panel, LV_GRID_ALIGN_STRETCH, j, 1, LV_GRID_ALIGN_STRETCH, i, 1);
            
            // Áp dụng style mặc định (style_cell)
            lv_obj_add_style(cell_panel, &style_cell, 0); 
            // Áp dụng style nhấn (style_pressed) cho trạng thái PRESSED
            lv_obj_add_style(cell_panel, &style_pressed, LV_STATE_PRESSED); 
            
            // Thêm sự kiện click và truyền chỉ số (0-99)
            uintptr_t index = i * BOARD_SIZE + j; 
            lv_obj_add_event_cb(cell_panel, game_cell_cb, LV_EVENT_CLICKED, (void*)index);

            // lưu panel để đổi màu sau này
            cell_panels[i][j] = cell_panel;

            // label (không hiển thị X/O nữa, nhưng để dự phòng)
            cells[i][j] = lv_label_create(cell_panel);
            lv_label_set_text(cells[i][j], "");
            lv_obj_center(cells[i][j]); 
            lv_obj_set_style_text_font(cells[i][j], &lv_font_montserrat_16, 0);
        }
    }
    
    // 7. Khởi động game
    reset_game(); 
}

/**
 * @brief Được gọi khi nhấn nút "Chơi Lại"
 */
static void reset_button_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        DEBUG_PRINTLN("Nhấn Chơi Lại!");
        reset_game();
    }
}

/**
 * @brief Được gọi khi nhấn vào một ô cờ
 */
static void game_cell_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);// Ép kiểu từ void* sang lv_obj_t*
    
    // Lấy dữ liệu user_data (chứa chỉ số 0-99) 
    uintptr_t index = (uintptr_t)lv_event_get_user_data(e);
    int row = index / BOARD_SIZE;
    int col = index % BOARD_SIZE;

    if (code == LV_EVENT_CLICKED && !game_over && board[row][col] == ' ') {
        
        // Cập nhật logic
        board[row][col] = currentPlayer;

        // Cập nhật UI: xóa label text (không hiển thị X/O), thay bằng tô nền
        lv_label_set_text(cells[row][col], "");
        if (currentPlayer == 'X') {
            // remove O style if any, apply X bg style
            lv_obj_remove_style(cell_panels[row][col], &style_o_bg, 0);
            lv_obj_add_style(cell_panels[row][col], &style_x_bg, 0);
        } else {
            lv_obj_remove_style(cell_panels[row][col], &style_x_bg, 0);
            lv_obj_add_style(cell_panels[row][col], &style_o_bg, 0);
        }

        // Kiểm tra thắng/thua/hòa
        char winner = check_win_and_fill_positions();
        if (winner != ' ') {
            game_over = true;
            if (winner == 'X') score_x++;
            if (winner == 'O') score_o++;
            update_score_labels();

            if (winner == 'D') {
                lv_label_set_text(status_label, "DUEL!");
            } else {
                char buf[20];
                sprintf(buf, "%c WIN!", winner);
                lv_label_set_text(status_label, buf);
                // Start blinking winning positions
                if (win_positions_valid) {
                    // tạo timer (200ms) nếu chưa có
                    if (blink_timer) lv_timer_del(blink_timer);
                    blink_state = false;
                    blink_timer = lv_timer_create(blink_timer_cb, 300, NULL); // 300ms toggle
                   
                }
            }
        } else {
            // Đổi lượt
            currentPlayer = (currentPlayer == 'X') ? 'O' : 'X';
            char buf[20];
            sprintf(buf, "%c Turn", currentPlayer);
            lv_label_set_text(status_label, buf);
        }
        
    }
}

void setup ()
{
    Serial.begin( 115200 ); 
    pinMode(resetPin, INPUT_PULLUP);
    delay(5000);    
    DEBUG_PRINTLN( "Starting ..." );
    
     if (!screenSetup()) {      
         DEBUG_PRINTLN("Failed to initialize JC3248W535EN LCD!");
         while (1);
     }
    DEBUG_PRINTLN("JC3248W535EN LCD Initialized.");
    
    lvgl_mutex = xSemaphoreCreateMutex();
    if (lvgl_mutex == NULL) {
      DEBUG_PRINTLN("Failed to create lvgl_mutex!");
      while (1); // Hoặc xử lý lỗi phù hợp
    }

    // #######################################################
    // ### GỌI HÀM TẠO GAME Ở ĐÂY ###
    DEBUG_PRINTLN("Creating Tic-Tac-Toe UI...");
    create_tic_tac_toe_ui(); 
    DEBUG_PRINTLN("UI Created.");
    // #######################################################

    xTaskCreatePinnedToCore(lvglTask, "LVGL Task", 8192, NULL, 1, NULL, 0);// core 0
    DEBUG_PRINTLN("Go loop...");
}

void loop ()// loop wifi core 1
{
    yield();
}
