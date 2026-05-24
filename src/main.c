#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <string.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include "font.h"

#define LUX_THRESHOLD_DARK 50.0 

typedef struct {
    int current_brightness; 
    int manual_target;      // Giá trị đích khi ở chế độ Manual (từ Web)
    bool motion_detected;
    float ambient_lux;
    bool manual_override;
    char current_time[32];
    // --- Thông tin trạng thái logic ---
    bool is_dark_confirmed;   // Trời tối + lux thấp
    bool is_night;            // 19h-6h
    bool human_present;       // Đã xác nhận có người
    int no_motion_ticks;      // Số tick không có chuyển động
    int auto_target;          // Giá trị đích tự động
    // --- Energy Management ---
    int energy_seconds[24]; // Tổng số giây đèn sáng cho mỗi giờ (0-23)
    int total_on_off_count; // Tổng số lần bật/tắt đèn
    pthread_mutex_t lock;
} SystemState;

SystemState g_state;

// ---------------------------------------------------------
// CÁC HÀM GIAO TIẾP VỚI KERNEL DRIVERS 
// ---------------------------------------------------------
bool read_pir_from_driver(void) {
    char buf[2] = {0};
    int fd = open("/dev/mypir", O_RDONLY);
    if (fd < 0) return false;
    read(fd, buf, sizeof(buf)); 
    close(fd);
    return (buf[0] == '1');
}

float read_lux_from_driver(void) {
    char buf[16] = {0};
    int fd = open("/dev/bh1750", O_RDONLY);
    if (fd < 0) return 0.0;
    read(fd, buf, sizeof(buf)-1);
    close(fd);
    return (float)atoi(buf);
}

void write_led_to_driver(int brightness) {
    char buf[16];
    int fd = open("/sys/class/smart_lamp/led/brightness", O_WRONLY);
    if (fd >= 0) {
        snprintf(buf, sizeof(buf), "%d\n", brightness);
        write(fd, buf, strlen(buf));
        close(fd);
    }
}

void read_rtc_from_driver(char *buf) {
    int fd = open("/dev/myds3231", O_RDONLY);
    if (fd >= 0) {
        int n = read(fd, buf, 31);
        if (n > 0) {
            buf[n] = '\0'; // Đảm bảo kết thúc chuỗi
            // Xóa ký tự \n ở cuối nếu có
            if (buf[n-1] == '\n') buf[n-1] = '\0';
        } else {
            strcpy(buf, "N/A");
        }
        close(fd);
    } else {
        strcpy(buf, "Error");
    }
}

// ---------------------------------------------------------
// THREAD 1: Đọc cảm biến
// ---------------------------------------------------------
void* sensor_thread_func(void* arg) {
    (void)arg;
    while (1) {
        bool motion = read_pir_from_driver();
        float lux = read_lux_from_driver();
        char time_buf[32];
        read_rtc_from_driver(time_buf);
        
        pthread_mutex_lock(&g_state.lock);
        g_state.motion_detected = motion;
        g_state.ambient_lux = lux;
        strcpy(g_state.current_time, time_buf);
        pthread_mutex_unlock(&g_state.lock);
        
        usleep(200000); // 200ms
    }
    return NULL;
}

// ---------------------------------------------------------
// THREAD 2: Logic điều khiển (Fading) + Energy Logging
// ---------------------------------------------------------

// Lấy giờ hiện tại (ưu tiên chuỗi RTC, fallback sang system time)
int get_current_hour(const char *time_str) {
    // Thử parse từ chuỗi RTC: "YYYY-MM-DD HH:MM:SS"
    const char *space = strchr(time_str, ' ');
    if (space && strlen(space) > 3) {
        int h = atoi(space + 1);
        if (h >= 0 && h < 24) return h;
    }
    // Fallback: dùng system time
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    return t ? t->tm_hour : 0;
}

void* control_thread_func(void* arg) {
    (void)arg;
    int current_pwm_percent = 0;
    bool was_on = false;
    int energy_tick = 0;

    int motion_ticks = 0;
    int no_motion_ticks = 0;
    bool human_present = false;

    // Bắt buộc stdout flush ngay để log hiện trên minicom
    setvbuf(stdout, NULL, _IONBF, 0);

    while (1) {
        pthread_mutex_lock(&g_state.lock);
        bool motion = g_state.motion_detected;
        float lux = g_state.ambient_lux;
        bool manual = g_state.manual_override;
        int manual_target = g_state.manual_target;
        char t_buf[32];
        strcpy(t_buf, g_state.current_time);
        pthread_mutex_unlock(&g_state.lock);

        int current_hour = get_current_hour(t_buf);
        bool is_dark_confirmed = ((current_hour >= 19 || current_hour < 6) && (lux < 200.0));

        int target;
        if (manual) {
            target = manual_target;
        } else {
            if (!is_dark_confirmed) {
                target = 0;
                human_present = false;
                motion_ticks = 0;
                no_motion_ticks = 0;
            } else {
                if (motion) {
                    motion_ticks++;
                    no_motion_ticks = 0;
                    if (motion_ticks >= 100) { // 100 * 20ms = 2s
                        human_present = true;
                    }
                } else {
                    no_motion_ticks++;
                    motion_ticks = 0;
                    if (no_motion_ticks >= 250) { // 250 * 20ms = 5s
                        human_present = false;
                    }
                }
                
                if (human_present) {
                    target = 100;
                } else {
                    target = 20; // Mức nền an ninh
                }
            }
        }

        // Fade effect
        if (current_pwm_percent < target) current_pwm_percent += 2;
        else if (current_pwm_percent > target) current_pwm_percent -= 2;
        
        if (current_pwm_percent < 0) current_pwm_percent = 0;
        if (current_pwm_percent > 100) current_pwm_percent = 100;

        write_led_to_driver(current_pwm_percent);

        // --- Energy Logging ---
        bool is_on = (current_pwm_percent > 0);

        // Chỉ đếm khi đèn chuyển từ TẮT -> SÁNG
        if (is_on && !was_on) {
            printf("[ENERGY LOG] Den BAT tai %s\n", t_buf);
            fflush(stdout);
            pthread_mutex_lock(&g_state.lock);
            g_state.total_on_off_count++;
            pthread_mutex_unlock(&g_state.lock);
        } else if (!is_on && was_on) {
            printf("[ENERGY LOG] Den TAT tai %s\n", t_buf);
            fflush(stdout);
        }
        was_on = is_on;

        // Cộng dồn thời gian sáng (mỗi 50 tick = 1 giây)
        if (is_on) {
            energy_tick++;
            if (energy_tick >= 50) { // 50 * 20ms = 1 giây
                energy_tick = 0;
                int hour = get_current_hour(t_buf);
                pthread_mutex_lock(&g_state.lock);
                g_state.energy_seconds[hour]++;
                pthread_mutex_unlock(&g_state.lock);
            }
        } else {
            energy_tick = 0;
        }

        pthread_mutex_lock(&g_state.lock);
        g_state.current_brightness = current_pwm_percent;
        g_state.is_dark_confirmed = is_dark_confirmed;
        g_state.is_night = (current_hour >= 19 || current_hour < 6);
        g_state.human_present = human_present;
        g_state.no_motion_ticks = no_motion_ticks;
        g_state.auto_target = target;
        pthread_mutex_unlock(&g_state.lock);

        usleep(20000); // 20ms
    }
    return NULL;
}

// ---------------------------------------------------------
// THREAD 3: Web Server (Raw Socket)
// ---------------------------------------------------------
void* web_server_thread(void* arg) {
    (void)arg;
    int server_fd, new_socket;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address)); // Fix: Initialize sockaddr_in to 0
    int opt = 1;
    int addrlen = sizeof(address);
    char buffer[2048] = {0};
    
    // Đọc giao diện HTML vào RAM
    char *html_content = NULL;
    long html_size = 0;
    FILE *fp = fopen("/root/www/index.html", "r");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        html_size = ftell(fp);
        rewind(fp);
        html_content = malloc(html_size + 1);
        fread(html_content, 1, html_size, fp);
        html_content[html_size] = '\0';
        fclose(fp);
    } else {
        printf("LỖI: Không tìm thấy file www/index.html!\n");
    }

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket failed");
        return NULL;
    }
    
    // Sửa lỗi: chỉ dùng SO_REUSEADDR
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("Setsockopt failed");
        return NULL;
    }
    
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(8080);
    
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        return NULL;
    }
    
    if (listen(server_fd, 5) < 0) {
        perror("Listen failed");
        return NULL;
    }
    
    printf("\n======================================================\n");
    printf(" 🚀 C WEB SERVER ĐANG CHẠY TẠI PORT 8080\n");
    printf(" Hãy mở trình duyệt và truy cập: http://<IP_Board>:8080\n");
    printf("======================================================\n\n");
    
    while (1) {
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) continue;
        
        // Thêm Timeout 1 giây để tránh trình duyệt mở connection ảo làm treo Server
        struct timeval tv;
        tv.tv_sec = 1; tv.tv_usec = 0;
        setsockopt(new_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        
        memset(buffer, 0, sizeof(buffer));
        if (read(new_socket, buffer, sizeof(buffer) - 1) <= 0) {
            close(new_socket);
            continue;
        }
        
        if (strncmp(buffer, "GET / ", 6) == 0) {
            char header[256];
            snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/html\r\nContent-Length: %ld\r\n\r\n", html_size);
            write(new_socket, header, strlen(header));
            if (html_content) write(new_socket, html_content, html_size);
        } 
        else if (strncmp(buffer, "GET /api/status", 15) == 0) {
            pthread_mutex_lock(&g_state.lock);
            int b = g_state.current_brightness;
            int m = g_state.motion_detected ? 1 : 0;
            int l = (int)g_state.ambient_lux;
            int man = g_state.manual_override ? 1 : 0;
            int is_dark = g_state.is_dark_confirmed ? 1 : 0;
            int is_night = g_state.is_night ? 1 : 0;
            int hp = g_state.human_present ? 1 : 0;
            int nmt = g_state.no_motion_ticks;
            int atarget = g_state.auto_target;
            char t_buf[32];
            strcpy(t_buf, g_state.current_time);
            pthread_mutex_unlock(&g_state.lock);
            
            char json[512];
            snprintf(json, sizeof(json),
                "{\"brightness\":%d,\"motion\":%d,\"lux\":%d,\"manual\":%d,\"time\":\"%s\","
                "\"is_dark\":%d,\"is_night\":%d,\"human_present\":%d,\"no_motion_ticks\":%d,\"auto_target\":%d}",
                b, m, l, man, t_buf, is_dark, is_night, hp, nmt, atarget);
            
            char header[256];
            snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: %lu\r\n\r\n", strlen(json));
            write(new_socket, header, strlen(header));
            write(new_socket, json, strlen(json));
        }
        else if (strncmp(buffer, "GET /api/energy", 15) == 0) {
            pthread_mutex_lock(&g_state.lock);
            int counts = g_state.total_on_off_count;
            char json[512];
            int pos = snprintf(json, sizeof(json), "{\"on_off_count\":%d,\"hours\":[", counts);
            for (int i = 0; i < 24; i++) {
                pos += snprintf(json + pos, sizeof(json) - pos, "%d%s", g_state.energy_seconds[i], i < 23 ? "," : "");
            }
            snprintf(json + pos, sizeof(json) - pos, "]}");
            pthread_mutex_unlock(&g_state.lock);
            
            char header[256];
            snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n", (int)strlen(json));
            write(new_socket, header, strlen(header));
            write(new_socket, json, strlen(json));
        }
        else if (strncmp(buffer, "GET /api/control", 16) == 0) {
            int man = 0, br = 0;
            char *m_ptr = strstr(buffer, "m=");
            char *b_ptr = strstr(buffer, "b=");
            if (m_ptr) man = atoi(m_ptr + 2);
            if (b_ptr) br = atoi(b_ptr + 2);
            
            pthread_mutex_lock(&g_state.lock);
            g_state.manual_override = (man == 1);
            if (g_state.manual_override) g_state.manual_target = br;
            pthread_mutex_unlock(&g_state.lock);
            
            char *resp = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n\r\nOK";
            write(new_socket, resp, strlen(resp));
        }
        else {
            char *resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
            write(new_socket, resp, strlen(resp));
        }
        close(new_socket);
    }
    
    if (html_content) free(html_content);
    return NULL;
}

// ---------------------------------------------------------
// THREAD 4: Màn hình OLED
// ---------------------------------------------------------
void draw_char(unsigned char *frame, int x, int y, char c) {
    if (c < 32 || c > 126) c = 32;
    if (x >= 128 || y >= 8) return;
    int c_idx = c - 32;
    for (int i = 0; i < 5; i++) {
        if (x + i < 128) frame[y * 128 + x + i] = font5x7[c_idx][i];
    }
}

void draw_string(unsigned char *frame, int x, int y, const char *str) {
    while (*str) {
        draw_char(frame, x, y, *str);
        x += 6;
        str++;
    }
}

void* oled_thread_func(void* arg) {
    (void)arg;
    int fd = open("/dev/myoled", O_WRONLY);
    if (fd < 0) {
        perror("Không thể mở /dev/myoled. OLED Thread thoát.");
        return NULL;
    }

    unsigned char frame[1024];

    while (1) {
        memset(frame, 0, sizeof(frame));

        pthread_mutex_lock(&g_state.lock);
        bool manual = g_state.manual_override;
        int target = g_state.manual_target;
        bool motion = g_state.motion_detected;
        float lux = g_state.ambient_lux;
        char t_buf[32];
        strcpy(t_buf, g_state.current_time);
        int current_b = g_state.current_brightness;
        pthread_mutex_unlock(&g_state.lock);

        char buf[32];
        
        // Dòng 0: Tiêu đề
        draw_string(frame, 15, 0, "SMART LAMP SYSTEM");
        for (int i = 0; i < 128; i++) frame[1 * 128 + i] |= 0x01; // Đường kẻ ngang

        // Dòng 2: Thời gian (Bỏ chữ TIME: đi vì màn hình chỉ chứa được tối đa 21 ký tự)
        int time_len = strlen(t_buf);
        int x_offset = (128 - (time_len * 6)) / 2; // Canh giữa
        if (x_offset < 0) x_offset = 0;
        draw_string(frame, x_offset, 2, t_buf);

        // Dòng 4: Cảm biến
        snprintf(buf, sizeof(buf), "LUX: %-4d MOT: %s", (int)lux, motion ? "YES" : "NO ");
        draw_string(frame, 0, 4, buf);

        // Dòng 6: Trạng thái đèn
        snprintf(buf, sizeof(buf), "LED: %-3d%% [%s]", current_b, manual ? "MAN" : "AUT");
        draw_string(frame, 0, 6, buf);

        write(fd, frame, sizeof(frame));
        usleep(1000000); // Cập nhật mỗi 1 giây
    }

    close(fd);
    return NULL;
}

int main() {
    g_state.current_brightness = 0;
    g_state.motion_detected = false;
    g_state.manual_override = false;
    g_state.ambient_lux = 0.0;
    g_state.is_dark_confirmed = false;
    g_state.is_night = false;
    g_state.human_present = false;
    g_state.no_motion_ticks = 0;
    g_state.auto_target = 0;
    g_state.total_on_off_count = 0;
    memset(g_state.energy_seconds, 0, sizeof(g_state.energy_seconds));
    pthread_mutex_init(&g_state.lock, NULL);

    pthread_t sensor_tid, control_tid, web_tid, oled_tid;

    pthread_create(&sensor_tid, NULL, sensor_thread_func, NULL);
    pthread_create(&control_tid, NULL, control_thread_func, NULL);
    pthread_create(&web_tid, NULL, web_server_thread, NULL);
    pthread_create(&oled_tid, NULL, oled_thread_func, NULL);

    pthread_join(sensor_tid, NULL);
    pthread_join(control_tid, NULL);
    pthread_join(web_tid, NULL);
    pthread_join(oled_tid, NULL);

    pthread_mutex_destroy(&g_state.lock);
    return 0;
}
