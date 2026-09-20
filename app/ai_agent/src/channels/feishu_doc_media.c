/****************************************************************************
 * packages/ai_agent/src/channels/feishu_doc_media.c
 * Feishu Document Media Operations (Images, Text, Dividers)
 * Uses tenant_access_token for consistency with document creation.
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <syslog.h>

#include "cJSON.h"
#include "channels/feishu_bot.h"
#include "channels/feishu_internal.h"
#include "infra/config_store.h"

static const char *TAG = "feishu_doc";
#define FORM_BOUNDARY "----DocMediaBoundary7MA4YWxkTrZu0gW"

/* 文档创建回调 */
static feishu_doc_created_cb_t s_doc_created_cb = NULL;
static void *s_doc_created_cb_userdata = NULL;

int feishu_set_doc_created_callback(feishu_doc_created_cb_t cb, void *user_data)
{
  s_doc_created_cb = cb;
  s_doc_created_cb_userdata = user_data;
  syslog(LOG_INFO, "[%s] Doc created callback %s\n", TAG, cb ? "registered" : "cleared");
  return OK;
}

/* 内部函数：文档创建成功后通知上层 */
void feishu_doc_notify_created(const char *doc_id, const char *title)
{
  if (s_doc_created_cb) {
    syslog(LOG_DEBUG, "[%s] Notifying doc created: id=%s title=%s\n", TAG, doc_id, title ? title : "");
    s_doc_created_cb(doc_id, title, s_doc_created_cb_userdata);
  } else {
    syslog(LOG_DEBUG, "[%s] No doc created callback registered, skip notification\n", TAG);
  }
}

/* ── Multipart body builder for media upload ─────────────────────── */

static char *build_doc_media_multipart(const char *doc_id, const char *image_block_id,
                                        const uint8_t *file_data, size_t file_size,
                                        size_t *out_len)
{
  const char *filename = "doc_image.jpg";
  char extra_json[256];
  char size_str[32];
  
  snprintf(extra_json, sizeof(extra_json), "{\"drive_route_token\":\"%s\"}", doc_id);
  snprintf(size_str, sizeof(size_str), "%zu", file_size);
  
  size_t total = 0;
  const char *part_fmt = "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n";
  const char *file_fmt = "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\nContent-Type: image/jpeg\r\n\r\n";
  const char *end_fmt = "--%s--\r\n";
  
  total += snprintf(NULL, 0, part_fmt, FORM_BOUNDARY, "file_name") + strlen(filename) + 2;
  total += snprintf(NULL, 0, part_fmt, FORM_BOUNDARY, "parent_type") + strlen("docx_image") + 2;
  total += snprintf(NULL, 0, part_fmt, FORM_BOUNDARY, "parent_node") + strlen(image_block_id) + 2;
  total += snprintf(NULL, 0, part_fmt, FORM_BOUNDARY, "size") + strlen(size_str) + 2;
  total += snprintf(NULL, 0, part_fmt, FORM_BOUNDARY, "extra") + strlen(extra_json) + 2;
  total += snprintf(NULL, 0, file_fmt, FORM_BOUNDARY, filename) + file_size + 2;
  total += snprintf(NULL, 0, end_fmt, FORM_BOUNDARY);
  
  char *body = malloc(total + 1);
  if (!body) return NULL;
  
  char *p = body;
  p += sprintf(p, part_fmt, FORM_BOUNDARY, "file_name");
  memcpy(p, filename, strlen(filename)); p += strlen(filename);
  memcpy(p, "\r\n", 2); p += 2;
  p += sprintf(p, part_fmt, FORM_BOUNDARY, "parent_type");
  memcpy(p, "docx_image", strlen("docx_image")); p += strlen("docx_image");
  memcpy(p, "\r\n", 2); p += 2;
  p += sprintf(p, part_fmt, FORM_BOUNDARY, "parent_node");
  memcpy(p, image_block_id, strlen(image_block_id)); p += strlen(image_block_id);
  memcpy(p, "\r\n", 2); p += 2;
  p += sprintf(p, part_fmt, FORM_BOUNDARY, "size");
  memcpy(p, size_str, strlen(size_str)); p += strlen(size_str);
  memcpy(p, "\r\n", 2); p += 2;
  p += sprintf(p, part_fmt, FORM_BOUNDARY, "extra");
  memcpy(p, extra_json, strlen(extra_json)); p += strlen(extra_json);
  memcpy(p, "\r\n", 2); p += 2;
  p += sprintf(p, file_fmt, FORM_BOUNDARY, filename);
  memcpy(p, file_data, file_size); p += file_size;
  memcpy(p, "\r\n", 2); p += 2;
  p += sprintf(p, end_fmt, FORM_BOUNDARY);
  
  *out_len = (size_t)(p - body);
  return body;
}

/* ── Helpers ─────────────────────────────────────────────────────── */

static int do_request(const char *method, const char *path,
                      const char *content_type,
                      const void *body, size_t body_len,
                      char **resp_out, long *status_out)
{
  syslog(LOG_DEBUG, "[%s] >>> %s %s body_len=%zu ct=%s\n", TAG, method, path, body_len, content_type ? content_type : "(none)");

  if (s_access_token[0] == '\0' || feishu_token_expired()) {
    syslog(LOG_DEBUG, "[%s] Token expired, refreshing...\n", TAG);
    if (feishu_get_app_token() != OK) {
      syslog(LOG_ERR, "[%s] Token refresh failed\n", TAG);
      return -EACCES;
    }
    syslog(LOG_DEBUG, "[%s] Token refreshed successfully\n", TAG);
  }
  
  char auth_val[520];
  snprintf(auth_val, sizeof(auth_val), "Bearer %s", s_access_token);
  
  vela_header_t headers[3] = {
    { "Authorization", auth_val },
    { NULL, NULL },
    { NULL, NULL }
  };
  int hdr_idx = 1;
  if (content_type) {
    headers[hdr_idx].name = "Content-Type";
    headers[hdr_idx].value = content_type;
    hdr_idx++;
  }
  
  char *resp = malloc(8192);
  if (!resp) {
    syslog(LOG_ERR, "[%s] OOM for response buffer\n", TAG);
    return -ENOMEM;
  }
  memset(resp, 0, 8192);
  size_t resp_len = 0;
  
  int status = feishu_https_request(method, path, headers,
                                    body, body_len, resp, 8192, &resp_len);
  
  if (status_out) *status_out = (long)status;
  
  syslog(LOG_DEBUG, "[%s] <<< %s %s status=%d resp_len=%zu\n", TAG, method, path, status, resp_len);
  
  if (status == 200) {
    if (resp_out) {
      *resp_out = resp;
      syslog(LOG_DEBUG, "[%s] Response (truncated): %.300s\n", TAG, resp);
    } else {
      free(resp);
    }
    return OK;
  } else {
    syslog(LOG_ERR, "[%s] %s %s failed: HTTP %ld resp: %.500s\n",
           TAG, method, path, (long)status, resp);
    free(resp);
    return -EIO;
  }
}

static int extract_first_block_id(const char *json, char *out, size_t out_cap)
{
  cJSON *root = cJSON_Parse(json);
  if (!root) return -EINVAL;
  
  cJSON *code = cJSON_GetObjectItem(root, "code");
  if (!code || code->valueint != 0) {
    cJSON *msg = cJSON_GetObjectItem(root, "msg");
    syslog(LOG_ERR, "[%s] API error: code=%lld msg=%s\n", TAG,
           code ? (long long)code->valueint : -1LL, msg ? msg->valuestring : "?");
    cJSON_Delete(root);
    return -EIO;
  }
  
  cJSON *data = cJSON_GetObjectItem(root, "data");
  cJSON *children = data ? cJSON_GetObjectItem(data, "children") : NULL;
  cJSON *first = children ? cJSON_GetArrayItem(children, 0) : NULL;
  cJSON *block_id = first ? cJSON_GetObjectItem(first, "block_id") : NULL;
  
  if (!block_id || !cJSON_IsString(block_id)) {
    syslog(LOG_ERR, "[%s] No block_id in response\n", TAG);
    cJSON_Delete(root);
    return -ENOENT;
  }
  
  strncpy(out, block_id->valuestring, out_cap - 1);
  out[out_cap - 1] = '\0';
  cJSON_Delete(root);
  return OK;
}

static int extract_file_token(const char *json, char *out, size_t out_cap)
{
  cJSON *root = cJSON_Parse(json);
  if (!root) return -EINVAL;
  
  cJSON *code = cJSON_GetObjectItem(root, "code");
  if (!code || code->valueint != 0) {
    cJSON *msg = cJSON_GetObjectItem(root, "msg");
    syslog(LOG_ERR, "[%s] Upload API error: code=%lld msg=%s\n", TAG,
           code ? (long long)code->valueint : -1LL, msg ? msg->valuestring : "?");
    cJSON_Delete(root);
    return -EIO;
  }
  
  cJSON *data = cJSON_GetObjectItem(root, "data");
  cJSON *token = data ? cJSON_GetObjectItem(data, "file_token") : NULL;
  
  if (!token || !cJSON_IsString(token)) {
    syslog(LOG_ERR, "[%s] No file_token in response\n", TAG);
    cJSON_Delete(root);
    return -ENOENT;
  }
  
  strncpy(out, token->valuestring, out_cap - 1);
  out[out_cap - 1] = '\0';
  cJSON_Delete(root);
  return OK;
}

/* ── Public API ──────────────────────────────────────────────────── */

int feishu_doc_append_divider(const char *doc_id)
{
  syslog(LOG_DEBUG, "[%s] Appending divider to doc %s\n", TAG, doc_id);

  char path[1024];
  snprintf(path, sizeof(path),
    "/open-apis/docx/v1/documents/%s/blocks/%s/children", doc_id, doc_id);
  
  cJSON *root = cJSON_CreateObject();
  cJSON *children = cJSON_AddArrayToObject(root, "children");
  cJSON *block = cJSON_CreateObject();
  cJSON_AddNumberToObject(block, "block_type", 22);
  cJSON *divider = cJSON_AddObjectToObject(block, "divider");
  cJSON_AddNumberToObject(divider, "wrap", 1);
  cJSON_AddItemToArray(children, block);
  
  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!body) return -ENOMEM;
  
  long status = 0;
  int ret = do_request("POST", path, "application/json; charset=utf-8",
                       body, strlen(body), NULL, &status);
  free(body);
  
  if (ret == OK) {
    syslog(LOG_INFO, "[%s] Divider appended to %s\n", TAG, doc_id);
  }
  return ret;
}

int feishu_doc_append_heading1(const char *doc_id, const char *title)
{
  if (!doc_id || doc_id[0] == '\0' || !title || title[0] == '\0') {
    syslog(LOG_WARNING, "[%s] append_heading1 called with invalid params\n", TAG);
    return -EINVAL;
  }

  syslog(LOG_INFO, "[%s] Appending heading to doc %s: %s\n", TAG, doc_id, title);

  char path[1024];
  snprintf(path, sizeof(path),
    "/open-apis/docx/v1/documents/%s/blocks/%s/children", doc_id, doc_id);

  cJSON *root = cJSON_CreateObject();
  cJSON *children = cJSON_AddArrayToObject(root, "children");
  cJSON *block = cJSON_CreateObject();
  cJSON_AddNumberToObject(block, "block_type", 5);

  cJSON *heading = cJSON_AddObjectToObject(block, "heading3");
  cJSON *elements = cJSON_AddArrayToObject(heading, "elements");
  cJSON *elem = cJSON_CreateObject();
  cJSON *tr = cJSON_AddObjectToObject(elem, "text_run");
  cJSON_AddStringToObject(tr, "content", title);
  cJSON_AddItemToArray(elements, elem);
  cJSON_AddObjectToObject(heading, "style");

  cJSON_AddItemToArray(children, block);

  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!body) return -ENOMEM;

  long status = 0;
  int ret = do_request("POST", path, "application/json; charset=utf-8",
                       body, strlen(body), NULL, &status);
  free(body);

  if (ret == OK) {
    syslog(LOG_INFO, "[%s] Heading appended to %s: %s\n", TAG, doc_id, title);
  }
  return ret;
}

int feishu_doc_append_text(const char *doc_id, const char *text)
{
  if (!doc_id || doc_id[0] == '\0' || !text || text[0] == '\0') {
    syslog(LOG_WARNING, "[%s] append_text called with invalid params: doc_id=%p text=%p\n", TAG, doc_id, text);
    return -EINVAL;
  }

  syslog(LOG_INFO, "[%s] Starting text append to doc %s: len=%zu\n", TAG, doc_id, strlen(text));
  syslog(LOG_DEBUG, "[%s] Text content (truncated): %.200s\n", TAG, text);

  char path[1024];
  snprintf(path, sizeof(path),
    "/open-apis/docx/v1/documents/%s/blocks/%s/children", doc_id, doc_id);
  
  cJSON *root = cJSON_CreateObject();
  cJSON *children = cJSON_AddArrayToObject(root, "children");
  
  char *text_copy = strdup(text);
  if (!text_copy) {
    cJSON_Delete(root);
    return -ENOMEM;
  }
  
  char *saveptr = NULL;
  char *line = strtok_r(text_copy, "\n", &saveptr);
  int block_count = 0;
  
  while (line) {
    while (*line == ' ' || *line == '\t') line++;
    
    cJSON *block = cJSON_CreateObject();
    cJSON_AddNumberToObject(block, "block_type", 2);
    
    cJSON *text_obj = cJSON_AddObjectToObject(block, "text");
    cJSON *elements = cJSON_AddArrayToObject(text_obj, "elements");
    cJSON *elem = cJSON_CreateObject();
    cJSON *tr = cJSON_AddObjectToObject(elem, "text_run");
    cJSON_AddStringToObject(tr, "content", line);
    cJSON_AddItemToArray(elements, elem);
    cJSON_AddObjectToObject(text_obj, "style");
    
    cJSON_AddItemToArray(children, block);
    block_count++;
    
    line = strtok_r(NULL, "\n", &saveptr);
  }
  free(text_copy);
  
  if (block_count == 0) {
    cJSON_Delete(root);
    return 0;
  }
  
  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!body) return -ENOMEM;
  
  long status = 0;
  int ret = do_request("POST", path, "application/json; charset=utf-8",
                       body, strlen(body), NULL, &status);
  free(body);
  
  if (ret == OK) {
    syslog(LOG_INFO, "[%s] Appended %d text block(s) to %s\n", TAG, block_count, doc_id);
  }
  return ret;
}

int feishu_doc_create_image_block(const char *doc_id, char *image_block_id_out, size_t out_cap)
{
  syslog(LOG_DEBUG, "[%s] Creating empty image block in doc %s\n", TAG, doc_id);

  char path[1024];
  snprintf(path, sizeof(path),
    "/open-apis/docx/v1/documents/%s/blocks/%s/children", doc_id, doc_id);
  
  cJSON *root = cJSON_CreateObject();
  cJSON *children = cJSON_AddArrayToObject(root, "children");
  cJSON *block = cJSON_CreateObject();
  cJSON_AddNumberToObject(block, "block_type", 27);
  cJSON *image = cJSON_AddObjectToObject(block, "image");
  /* 创建空图片块时不需要传file_token，后续通过PATCH接口关联上传后的file_token即可，传空字符串会导致400错误 */
  cJSON_AddNumberToObject(image, "width", 600);
  cJSON_AddNumberToObject(image, "height", 400);
  cJSON_AddItemToArray(children, block);
  
  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!body) return -ENOMEM;
  
  char *resp = NULL;
  long status = 0;
  int ret = do_request("POST", path, "application/json; charset=utf-8",
                       body, strlen(body), &resp, &status);
  free(body);
  
  if (ret == OK && resp) {
    ret = extract_first_block_id(resp, image_block_id_out, out_cap);
    if (ret == OK) {
      syslog(LOG_INFO, "[%s] Created image block: %s\n", TAG, image_block_id_out);
    }
    free(resp);
  }
  return ret;
}

int feishu_doc_upload_image_media(const char *doc_id, const char *image_block_id,
                                   const char *image_path, char *file_token_out, size_t out_cap)
{
  syslog(LOG_INFO, "[%s] Starting image upload: doc_id=%s block_id=%s path=%s\n", TAG, doc_id, image_block_id, image_path);
  
  struct stat st;
  if (stat(image_path, &st) != 0) {
    syslog(LOG_ERR, "[%s] Cannot stat image %s: %d\n", TAG, image_path, errno);
    return -errno;
  }
  syslog(LOG_DEBUG, "[%s] Image file size: %ld bytes\n", TAG, (long)st.st_size);
  
  int fd = open(image_path, O_RDONLY);
  if (fd < 0) {
    syslog(LOG_ERR, "[%s] Cannot open image %s: %d\n", TAG, image_path, errno);
    return -errno;
  }
  
  uint8_t *data = malloc(st.st_size);
  if (!data) {
    syslog(LOG_ERR, "[%s] OOM for image data: %ld bytes\n", TAG, (long)st.st_size);
    close(fd);
    return -ENOMEM;
  }
  
  ssize_t nread = read(fd, data, st.st_size);
  close(fd);
  if (nread != st.st_size) {
    syslog(LOG_ERR, "[%s] Read image %s failed: %zd vs %ld\n", TAG, image_path, nread, (long)st.st_size);
    free(data);
    return -EIO;
  }
  syslog(LOG_DEBUG, "[%s] Image file read complete\n", TAG);
  
  size_t body_len = 0;
  char *body = build_doc_media_multipart(doc_id, image_block_id, data, st.st_size, &body_len);
  free(data);
  
  if (!body) {
    syslog(LOG_ERR, "[%s] Build multipart failed\n", TAG);
    return -ENOMEM;
  }
  syslog(LOG_DEBUG, "[%s] Multipart body built, size=%zu\n", TAG, body_len);
  
  char ct[128];
  snprintf(ct, sizeof(ct), "multipart/form-data; boundary=%s", FORM_BOUNDARY);
  
  char *resp = NULL;
  long status = 0;
  int ret = do_request("POST", "/open-apis/drive/v1/medias/upload_all",
                       ct, body, body_len, &resp, &status);
  free(body);
  
  if (ret == OK && resp) {
    ret = extract_file_token(resp, file_token_out, out_cap);
    if (ret == OK) {
      syslog(LOG_INFO, "[%s] Image uploaded SUCCESS: file_token=%s\n", TAG, file_token_out);
    } else {
      syslog(LOG_ERR, "[%s] Failed to extract file_token from response\n", TAG);
    }
    free(resp);
  } else {
    syslog(LOG_ERR, "[%s] Upload request failed, ret=%d status=%ld\n", TAG, ret, status);
  }
  return ret;
}

int feishu_doc_patch_image_block(const char *doc_id, const char *image_block_id, const char *file_token)
{
  syslog(LOG_DEBUG, "[%s] Patching image block: doc_id=%s block_id=%s file_token=%s\n", TAG, doc_id, image_block_id, file_token);

  char path[1024];
  snprintf(path, sizeof(path),
    "/open-apis/docx/v1/documents/%s/blocks/%s?update_image_elements=replace_image",
    doc_id, image_block_id);
  
  cJSON *root = cJSON_CreateObject();
  cJSON *update = cJSON_AddObjectToObject(root, "replace_image");
  /* 正确格式：token直接是file_token字符串，不是包含file_token的对象 */
  cJSON_AddStringToObject(update, "token", file_token);
  
  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!body) return -ENOMEM;
  
  long status = 0;
  int ret = do_request("PATCH", path, "application/json; charset=utf-8",
                       body, strlen(body), NULL, &status);
  free(body);
  
  if (ret == OK) {
    syslog(LOG_INFO, "[%s] Image block %s patched with file_token %s\n", TAG, image_block_id, file_token);
  }
  return ret;
}

int feishu_doc_append_image(const char *doc_id, const char *image_path)
{
  syslog(LOG_INFO, "[%s] Starting one-stop image append: doc_id=%s path=%s\n", TAG, doc_id, image_path);
  
  char image_block_id[128] = {0};
  char file_token[128] = {0};
  int ret;
  
  syslog(LOG_DEBUG, "[%s] Step 1/3: Creating empty image block...\n", TAG);
  ret = feishu_doc_create_image_block(doc_id, image_block_id, sizeof(image_block_id));
  if (ret != OK) {
    syslog(LOG_ERR, "[%s] Step 1 failed: create image block ret=%d\n", TAG, ret);
    return ret;
  }
  syslog(LOG_DEBUG, "[%s] Step 1 done: image_block_id=%s\n", TAG, image_block_id);
  
  syslog(LOG_DEBUG, "[%s] Step 2/3: Uploading image media...\n", TAG);
  ret = feishu_doc_upload_image_media(doc_id, image_block_id, image_path, file_token, sizeof(file_token));
  if (ret != OK) {
    syslog(LOG_ERR, "[%s] Step 2 failed: upload media ret=%d\n", TAG, ret);
    return ret;
  }
  syslog(LOG_DEBUG, "[%s] Step 2 done: file_token=%s\n", TAG, file_token);
  
  syslog(LOG_DEBUG, "[%s] Step 3/3: Patching image block...\n", TAG);
  ret = feishu_doc_patch_image_block(doc_id, image_block_id, file_token);
  if (ret != OK) {
    syslog(LOG_ERR, "[%s] Step 3 failed: patch block ret=%d\n", TAG, ret);
    return ret;
  }
  
  syslog(LOG_INFO, "[%s] Image append completed SUCCESSFULLY\n", TAG);
  return ret;
}
