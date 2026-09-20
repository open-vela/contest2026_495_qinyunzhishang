#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <lvgl/lvgl.h>
#include <bluetooth.h>
#include <bt_hfp_hf.h>

#include "bt_call_handler.h"
#include "call_page.h"
#include "calling_page.h"

#define CALL_STATE_FILE "/tmp/bt_call_state"
#define BT_INIT_THREAD_STACK_SIZE 16384

static bool g_bt_available = false;
static lv_timer_t *g_bt_check_timer = NULL;
static lv_timer_t *g_call_state_timer = NULL;
static int g_bt_check_count = 0;

static bt_call_state_t g_call_state = BT_CALL_IDLE;
static char g_call_number[64] = {0};
static char g_call_name[64] = {0};

static bt_instance_t *g_bt_inst = NULL;
static void *g_hfp_cookie = NULL;
static pthread_t g_bt_init_thread;
static volatile bool g_bt_init_thread_running = false;
static bool g_peer_addr_valid = false;
static bt_address_t g_peer_addr;

static void sanitize_phone_number(const char *src, char *dst, size_t dst_size)
{
    if (src == NULL || dst == NULL || dst_size == 0)
    {
        if (dst && dst_size > 0)
            dst[0] = '\0';
        return;
    }
    
    /* 使用strnlen限制最大扫描长度，防止未终止的字符串导致越界读取
     * 修复bug：蓝牙协议栈返回的call->number可能是固定长度数组（非null-terminated），
     * 导致原代码扫描到尾部垃圾数据，造成号码末尾多出随机数字 */
    size_t src_len = strnlen(src, dst_size);
    
    size_t j = 0;
    for (size_t i = 0; i < src_len && j < dst_size - 1; i++)
    {
        char c = src[i];
        if ((c >= '0' && c <= '9') || c == '+' || c == '*' || c == '#')
            dst[j++] = c;
    }
    dst[j] = '\0';
}

static void write_call_state(const char *state, const char *number, const char *name)
{
    char clean_num[64] = {0};
    sanitize_phone_number(number, clean_num, sizeof(clean_num));

    int fd = open("/tmp/bt_call_state.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;

    char buf[256];
    int len;
    if (name && name[0] != '\0')
        len = snprintf(buf, sizeof(buf), "%s:%s:%s\n", state, clean_num, name);
    else if (clean_num[0] != '\0')
        len = snprintf(buf, sizeof(buf), "%s:%s\n", state, clean_num);
    else
        len = snprintf(buf, sizeof(buf), "%s\n", state);

    write(fd, buf, len);
    close(fd);
    rename("/tmp/bt_call_state.tmp", CALL_STATE_FILE);
}

static void hf_connection_state_cb(void *cookie, bt_address_t *addr, profile_connection_state_t state)
{
    LV_LOG_USER("[BT_CALL] HFP connection state: %d", state);
    if (state == PROFILE_STATE_CONNECTED && addr != NULL)
    {
        memcpy(&g_peer_addr, addr, sizeof(bt_address_t));
        g_peer_addr_valid = true;
    }
    else if (state == PROFILE_STATE_DISCONNECTED)
    {
        g_peer_addr_valid = false;
        write_call_state("IDLE", "", "");
    }
}

static void hf_audio_state_cb(void *cookie, bt_address_t *addr, hfp_audio_state_t state)
{
    LV_LOG_USER("[BT_CALL] Audio state: %d", state);
}

static void hf_call_state_changed_cb(void *cookie, bt_address_t *addr, hfp_current_call_t *call)
{
    if (call == NULL)
        return;

    /* 创建安全的号码和姓名副本（强制null-terminated）
     * 防御蓝牙协议栈返回固定长度数组而非标准C字符串的情况
     * Bug修复：解决号码末尾多出随机数字的问题 */
    char safe_number[64] = {0};
    if (call->number != NULL)
    {
        strncpy(safe_number, call->number, sizeof(safe_number) - 1);
        safe_number[sizeof(safe_number) - 1] = '\0';
    }
    
    char safe_name[64] = {0};
    if (call->name != NULL)
    {
        strncpy(safe_name, call->name, sizeof(safe_name) - 1);
        safe_name[sizeof(safe_name) - 1] = '\0';
    }

    LV_LOG_USER("[BT_CALL] Call state: %d, number: %s, name: %s",
                call->state, safe_number, safe_name);

    switch (call->state)
    {
    case HFP_HF_CALL_STATE_INCOMING:
        write_call_state("INCOMING", safe_number, safe_name);
        break;
    case HFP_HF_CALL_STATE_DIALING:
        write_call_state("DIALING", safe_number, safe_name);
        break;
    case HFP_HF_CALL_STATE_ALERTING:
        write_call_state("ALERTING", safe_number, safe_name);
        break;
    case HFP_HF_CALL_STATE_ACTIVE:
        write_call_state("ACTIVE", safe_number, safe_name);
        break;
    case HFP_HF_CALL_STATE_HELD:
        write_call_state("HELD", safe_number, safe_name);
        break;
    case HFP_HF_CALL_STATE_DISCONNECTED:
        write_call_state("IDLE", "", "");
        break;
    default:
        break;
    }
}

static void hf_call_cb(void *cookie, bt_address_t *addr, hfp_call_t call)
{
    LV_LOG_USER("[BT_CALL] Call indicator: %d", call);
    if (call == HFP_CALL_NO_CALLS_IN_PROGRESS)
        write_call_state("IDLE", "", "");
}

static void hf_callsetup_cb(void *cookie, bt_address_t *addr, hfp_callsetup_t callsetup)
{
    LV_LOG_USER("[BT_CALL] Callsetup indicator: %d", callsetup);
    if (callsetup == HFP_CALLSETUP_INCOMING)
        write_call_state("INCOMING", "", "");
}

static void hf_callheld_cb(void *cookie, bt_address_t *addr, hfp_callheld_t callheld)
{
    LV_LOG_USER("[BT_CALL] Callheld indicator: %d", callheld);
}

static void hf_ring_indication_cb(void *cookie, bt_address_t *addr, bool inband_ring_tone)
{
    LV_UNUSED(addr);
    LV_LOG_USER("[BT_CALL] Ring indication, inband=%d", inband_ring_tone);
}

static void hf_volume_changed_cb(void *cookie, bt_address_t *addr, hfp_volume_type_t type, uint8_t volume)
{
    LV_LOG_USER("[BT_CALL] Volume changed: type=%d, vol=%d", type, volume);
}

static const hfp_hf_callbacks_t g_hfp_cbs = {
    sizeof(g_hfp_cbs),
    hf_connection_state_cb,
    hf_audio_state_cb,
    NULL,
    hf_call_state_changed_cb,
    NULL,
    hf_ring_indication_cb,
    hf_volume_changed_cb,
    hf_call_cb,
    hf_callsetup_cb,
    hf_callheld_cb,
};

static bool check_bt_service(void)
{
    int fd = socket(AF_LOCAL, SOCK_STREAM, 0);
    if (fd < 0)
        return false;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_LOCAL;
    snprintf(addr.sun_path, UNIX_PATH_MAX, "bt:bluetooth");

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(fd);
        return false;
    }

    close(fd);
    return true;
}

static void ensure_state_file_exists(void)
{
    int fd = open(CALL_STATE_FILE, O_RDONLY);
    if (fd >= 0)
    {
        close(fd);
        return;
    }

    fd = open(CALL_STATE_FILE, O_WRONLY | O_CREAT, 0644);
    if (fd >= 0)
    {
        write(fd, "IDLE\n", 5);
        close(fd);
        LV_LOG_USER("[BT_CALL] Created state file with IDLE");
    }
}

static bool init_bluetooth_hfp_internal(void)
{
    LV_LOG_USER("[BT_CALL] create_instance");
    g_bt_inst = bluetooth_create_instance();
    if (g_bt_inst == NULL)
    {
        LV_LOG_USER("[BT_CALL] Failed to create bluetooth instance");
        return false;
    }
    LV_LOG_USER("[BT_CALL] create_instance OK, instance=%p", g_bt_inst);

    g_hfp_cookie = bt_hfp_hf_register_callbacks(g_bt_inst, &g_hfp_cbs);
    if (g_hfp_cookie == NULL)
    {
        LV_LOG_USER("[BT_CALL] Failed to register HFP HF callbacks");
        bluetooth_delete_instance(g_bt_inst);
        g_bt_inst = NULL;
        return false;
    }
    LV_LOG_USER("[BT_CALL] HFP HF callbacks registered, cookie=%p", g_hfp_cookie);
    return true;
}

static void deinit_bluetooth_hfp(void)
{
    if (g_hfp_cookie != NULL && g_bt_inst != NULL)
    {
        bt_hfp_hf_unregister_callbacks(g_bt_inst, g_hfp_cookie);
        g_hfp_cookie = NULL;
    }

    if (g_bt_inst != NULL)
    {
        bluetooth_delete_instance(g_bt_inst);
        g_bt_inst = NULL;
    }
}

static void *bt_init_thread_func(void *arg)
{
    LV_LOG_USER("[BT_CALL] BT init thread started (stack=%d)", BT_INIT_THREAD_STACK_SIZE);
    bool ok = init_bluetooth_hfp_internal();
    if (ok)
    {
        g_bt_available = true;
        write_call_state("IDLE", "", "");
        LV_LOG_USER("[BT_CALL] BT init thread done, HFP ready");
    }
    else
    {
        g_bt_available = false;
        LV_LOG_USER("[BT_CALL] BT init thread done, HFP failed");
    }
    g_bt_init_thread_running = false;
    return NULL;
}

static bool create_bt_init_thread(void)
{
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0)
        return false;
    pthread_attr_setstacksize(&attr, BT_INIT_THREAD_STACK_SIZE);

    g_bt_init_thread_running = true;
    int ret = pthread_create(&g_bt_init_thread, &attr, bt_init_thread_func, NULL);
    pthread_attr_destroy(&attr);

    if (ret != 0)
    {
        LV_LOG_USER("[BT_CALL] Failed to create BT init thread: %d", ret);
        g_bt_init_thread_running = false;
        return false;
    }
    return true;
}

static void parse_call_state(const char *buf)
{
    if (buf == NULL || buf[0] == '\0')
        return;

    char line[256];
    strncpy(line, buf, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';

    char *newline = strchr(line, '\n');
    if (newline) *newline = '\0';

    char *state_str = line;
    char *number_str = NULL;
    char *name_str = NULL;

    char *colon1 = strchr(line, ':');
    if (colon1)
    {
        *colon1 = '\0';
        number_str = colon1 + 1;

        char *colon2 = strchr(number_str, ':');
        if (colon2)
        {
            *colon2 = '\0';
            name_str = colon2 + 1;
        }
    }

    if (strcmp(state_str, "INCOMING") == 0)
        g_call_state = BT_CALL_INCOMING;
    else if (strcmp(state_str, "DIALING") == 0)
        g_call_state = BT_CALL_DIALING;
    else if (strcmp(state_str, "ALERTING") == 0)
        g_call_state = BT_CALL_ALERTING;
    else if (strcmp(state_str, "ACTIVE") == 0)
        g_call_state = BT_CALL_ACTIVE;
    else if (strcmp(state_str, "HELD") == 0)
        g_call_state = BT_CALL_HELD;
    else
        g_call_state = BT_CALL_IDLE;

    if (number_str)
    {
        sanitize_phone_number(number_str, g_call_number, sizeof(g_call_number));
    }
    else
    {
        g_call_number[0] = '\0';
    }

    if (name_str)
    {
        strncpy(g_call_name, name_str, sizeof(g_call_name) - 1);
        g_call_name[sizeof(g_call_name) - 1] = '\0';
    }
    else
    {
        g_call_name[0] = '\0';
    }
}

static void read_call_state(void)
{
    int fd = open(CALL_STATE_FILE, O_RDONLY);
    if (fd < 0)
    {
        g_call_state = BT_CALL_IDLE;
        g_call_number[0] = '\0';
        g_call_name[0] = '\0';
        return;
    }

    char buf[256];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0)
    {
        g_call_state = BT_CALL_IDLE;
        g_call_number[0] = '\0';
        g_call_name[0] = '\0';
        return;
    }

    buf[n] = '\0';
    parse_call_state(buf);
}

static void call_state_timer_cb(lv_timer_t *timer)
{
    static bt_call_state_t last_state = BT_CALL_IDLE;
    read_call_state();

    if (g_call_state == BT_CALL_INCOMING && last_state != BT_CALL_INCOMING)
    {
        LV_LOG_USER("[BT_CALL] Auto-show incoming modal, num=%s", g_call_number);
        create_incoming_call_modal(g_call_number, g_call_name);
    }
    else if ((g_call_state == BT_CALL_ACTIVE ||
              g_call_state == BT_CALL_DIALING ||
              g_call_state == BT_CALL_ALERTING) &&
             last_state != g_call_state)
    {
        LV_LOG_USER("[BT_CALL] Auto-show calling page, state=%d, num=%s", g_call_state, g_call_number);
        show_calling_page();
        update_calling_info(g_call_number, g_call_name);
        update_calling_status((int)g_call_state);
        if (g_call_state == BT_CALL_ACTIVE)
            reset_calling_timer();
    }
    
    else if (g_call_state == BT_CALL_IDLE && last_state != BT_CALL_IDLE)
    {
        LV_LOG_USER("[BT_CALL] Auto-hide call ui");
        hide_call_ui();
    }

    last_state = g_call_state;
}

static void bt_check_timer_cb(lv_timer_t *timer)
{
    g_bt_check_count++;

    bool available = check_bt_service();

    if (available)
    {
        lv_timer_del(g_bt_check_timer);
        g_bt_check_timer = NULL;
        LV_LOG_USER("[BT_CALL] Bluetooth service is AVAILABLE (check #%d)", g_bt_check_count);

        ensure_state_file_exists();

        if (!g_bt_init_thread_running)
        {
            if (!create_bt_init_thread())
                g_bt_available = false;
        }

        g_call_state_timer = lv_timer_create(call_state_timer_cb, 2000, NULL);
        LV_LOG_USER("[BT_CALL] Call state polling started");
        return;
    }

    LV_LOG_USER("[BT_CALL] Bluetooth service not ready (check #%d/10)", g_bt_check_count);

    if (g_bt_check_count >= 10)
    {
        lv_timer_del(g_bt_check_timer);
        g_bt_check_timer = NULL;
        LV_LOG_USER("[BT_CALL] Gave up after 10 checks, BT service not available");
    }
}

static void delayed_bt_init_cb(lv_timer_t *timer)
{
    LV_LOG_USER("[BT_CALL] Delayed BT init triggered");
    lv_timer_del(timer);

    ensure_state_file_exists();

    bool available = check_bt_service();
    if (available)
    {
        LV_LOG_USER("[BT_CALL] Bluetooth service is available");

        if (!g_bt_init_thread_running)
        {
            if (!create_bt_init_thread())
                g_bt_available = false;
        }

        g_call_state_timer = lv_timer_create(call_state_timer_cb, 2000, NULL);
        LV_LOG_USER("[BT_CALL] Call state polling started");
        return;
    }

    g_bt_available = false;
    g_bt_check_timer = lv_timer_create(bt_check_timer_cb, 3000, NULL);
    lv_timer_set_repeat_count(g_bt_check_timer, 10);
    LV_LOG_USER("[BT_CALL] BT not ready, will check every 3s (max 10 times)");
}

int bt_call_handler_init(void)
{
    LV_LOG_USER("[BT_CALL] bt_call_handler_init called, will init in 5s");
    lv_timer_create(delayed_bt_init_cb, 5000, NULL);
    return 0;
}

void bt_call_handler_deinit(void)
{
    if (g_bt_check_timer != NULL)
    {
        lv_timer_del(g_bt_check_timer);
        g_bt_check_timer = NULL;
    }

    if (g_call_state_timer != NULL)
    {
        lv_timer_del(g_call_state_timer);
        g_call_state_timer = NULL;
    }

    deinit_bluetooth_hfp();

    LV_LOG_USER("[BT_CALL] bt_call_handler_deinit called");
    g_bt_available = false;
    g_call_state = BT_CALL_IDLE;
}

bool bt_call_handler_is_available(void)
{
    return g_bt_available;
}

bt_call_state_t bt_call_handler_get_call_state(void)
{
    return g_call_state;
}

const char *bt_call_handler_get_call_number(void)
{
    return g_call_number;
}

const char *bt_call_handler_get_call_name(void)
{
    return g_call_name;
}

int bt_call_accept(void)
{
    if (g_bt_inst == NULL || !g_peer_addr_valid)
    {
        LV_LOG_USER("[BT_CALL] accept: not connected");
        return -1;
    }
    bt_status_t status = bt_hfp_hf_accept_call(g_bt_inst, &g_peer_addr, HFP_HF_CALL_ACCEPT_NONE);
    LV_LOG_USER("[BT_CALL] accept_call status: %d", status);
    return (status == BT_STATUS_SUCCESS) ? 0 : -1;
}

int bt_call_reject(void)
{
    if (g_bt_inst == NULL || !g_peer_addr_valid)
    {
        LV_LOG_USER("[BT_CALL] reject: not connected");
        return -1;
    }
    bt_status_t status = bt_hfp_hf_reject_call(g_bt_inst, &g_peer_addr);
    LV_LOG_USER("[BT_CALL] reject_call status: %d", status);
    return (status == BT_STATUS_SUCCESS) ? 0 : -1;
}

int bt_call_terminate(void)
{
    if (g_bt_inst == NULL || !g_peer_addr_valid)
    {
        LV_LOG_USER("[BT_CALL] terminate: not connected");
        return -1;
    }
    bt_status_t status = bt_hfp_hf_terminate_call(g_bt_inst, &g_peer_addr);
    LV_LOG_USER("[BT_CALL] terminate_call status: %d", status);
    return (status == BT_STATUS_SUCCESS) ? 0 : -1;
}