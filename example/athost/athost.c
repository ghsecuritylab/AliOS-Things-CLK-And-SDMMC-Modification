/*
 * Copyright (C) 2015-2017 Alibaba Group Holding Limited
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <aos/aos.h>
#include <atparser.h>
#include <aos/network.h>
#include <hal/wifi.h>

#include "athost.h"

#define TAG "athost"

#define MAX_ATCMD_PREFIX 32
#define LINK_ID_MAX 5
#define MAX_RECV_BUF_SIZE 1500

static link_t g_link[LINK_ID_MAX];
static aos_mutex_t g_link_mutex;
static hal_wifi_event_cb_t wifi_events;
//static aos_sem_t start_sem;

static bool ip_ready = 0;
static bool inited = false;
static bool uart_echo_on = false;

static const char *prefix_athost = "AT+";
static const char *prefix_cipevent = "+CIPEVENT:";
static const char *prefix_cipdomain = "AT+CIPDOMAIN:";
static const char *prefix_wevent = "+WEVENT:";
static const char *prefix_wjap = "AT+WJAP";
static const char *prefix_wjapip = "AT+WJAPIP:";
static const char *prefix_wmac = "AT+WMAC";
static const char *conntype_str[] = { "tcp_server",
                                      "tcp_client",
                                      "ssl_client",
                                      "udp_broadcast",
                                      "udp_unicast"
                                    };
typedef struct {
    uint8_t *cmdptr;
    uint8_t *dataptr;
    uint16_t cmdlen;
    uint16_t datalen;
} uart_send_info_t;

typedef struct {
    uint32_t total_byte;
    uint32_t fetch_error;
    uint32_t put_error;
    uint32_t send_error;
} uart_send_stat_t;

#define DEFAULT_UART_SEND_BUF_SIZE 50
static aos_queue_t uart_send_queue;
static uart_send_stat_t uart_send_statistic;

typedef struct {
    int sockfd;
    uint8_t *dataptr;
    uint16_t datalen;
} sock_send_info_t;

typedef struct {
    uint32_t total_byte;
    uint32_t fetch_error;
    uint32_t put_error;
    uint32_t send_error;
} sock_send_stat_t;

#define DEFAULT_SOCK_SEND_BUF_SIZE 50
static aos_queue_t sock_send_queue;
static sock_send_stat_t sock_send_statistic;

static int notify_cip_connect_status_events(int sockid, int status, int recvstatus);
static int post_send_at_uart_task(const char *cmd);
static int post_send_socket_data_task(int sockid, const char *data, int datalen);
static int notify_atcmd_recv_status(int status);

static int uart_send_queue_init()
{
    uint32_t size = sizeof(uart_send_info_t) * DEFAULT_UART_SEND_BUF_SIZE;
    uart_send_info_t *uart_send_buf = NULL;

    memset(&uart_send_statistic, 0, sizeof(uart_send_statistic));

    uart_send_buf = (uart_send_info_t *) aos_malloc(size);
    if (!uart_send_buf) {
        LOGE(TAG, "uart send buf allocate %u fail!\r\n", size);
        goto err;
    }

    if (aos_queue_new(&uart_send_queue, uart_send_buf, size, sizeof(uart_send_info_t)) != 0) {
        LOGE(TAG, "uart send queue create fail!\r\n");
        goto err;
    }

    return 0;
err:
    aos_free(uart_send_buf);

    aos_queue_free(&uart_send_queue);

    return -1;
}

static int uart_send_queue_finalize()
{
    uart_send_info_t *uart_send_buf = NULL;

    if (!aos_queue_is_valid(&uart_send_queue)) {
        return -1;
    }

    uart_send_buf = (uart_send_info_t *) aos_queue_buf_ptr(&uart_send_queue);
    aos_free(uart_send_buf);

    aos_queue_free(&uart_send_queue);

    return 0;
}

void free_uart_send_msg(uart_send_info_t *msgptr)
{
    if (!msgptr) {
        return;
    }

    aos_free(msgptr->cmdptr);
    aos_free(msgptr->dataptr);
}

int insert_uart_send_msg(uint8_t *cmdptr, uint8_t *dataptr, uint16_t cmdlen, uint16_t datalen)
{
    uart_send_info_t uart_send_buf;

    if (!cmdptr || !cmdlen) {
        return -1;
    }

    if (strlen((char *) cmdptr) != cmdlen) {
        LOGE(TAG, "Error: cmd len does not match\r\n");
        return -1;
    }

    if (dataptr && !datalen) {
        return -1;
    }

    if (!aos_queue_is_valid(&uart_send_queue)) {
        return -1;
    }

    memset(&uart_send_buf, 0, sizeof(uart_send_info_t));
    uart_send_buf.cmdptr = (uint8_t *) aos_malloc(cmdlen + 1);
    if (!uart_send_buf.cmdptr) {
        LOGE(TAG, "uart send msg allocate fail\n");
        goto err;
    }

    LOGD(TAG, "insert cmd -->%s<-- len %d  addr %x to %x\n", cmdptr, cmdlen,
         uart_send_buf.cmdptr, uart_send_buf.cmdptr + cmdlen);
    memcpy(uart_send_buf.cmdptr, cmdptr, cmdlen);
    uart_send_buf.cmdptr[cmdlen] = 0;
    uart_send_buf.cmdlen = cmdlen;

    if (dataptr && datalen) {
        uart_send_buf.dataptr = (uint8_t *) aos_malloc(datalen);
        if (!uart_send_buf.dataptr) {
            LOGE(TAG, "Uart send msg allocate fail\n");
            goto err;
        }

        memcpy(uart_send_buf.dataptr, dataptr, datalen);
        uart_send_buf.datalen = datalen;
    }

    if (aos_queue_send(&uart_send_queue, &uart_send_buf, sizeof(uart_send_buf)) != 0) {
        LOGE(TAG, "Error: Uart queue send fail, total fail %d!\r\n", ++uart_send_statistic.put_error);
        goto err;
    }

    uart_send_statistic.total_byte += (cmdlen + datalen);
    LOGD(TAG, "uart cmdlen %d datalen %d total %d\n", cmdlen, datalen,
         uart_send_statistic.total_byte);

    return 0;

err:
    free_uart_send_msg(&uart_send_buf);
    return -1;
}

// return total byte sent
int send_over_uart(uart_send_info_t *msgptr)
{
    int ret;
    int size = 0;

    if (!msgptr) {
        return -1;
    }

    if (!msgptr->cmdptr && !msgptr->dataptr) {
        return -1;
    }

    if (strlen((char *) msgptr->cmdptr) != msgptr->cmdlen) {
        LOGE(TAG, "Error: cmd -->%s<-- len %d does not match!\r\n", msgptr->cmdptr, msgptr->cmdlen);
        return -1;
    }

    if (!msgptr->dataptr) {
        LOGD(TAG, "at going to send %s!\n", (char *)msgptr->cmdptr);

        ret = at.send_raw_no_rsp((char *)msgptr->cmdptr);
        if (ret != 0) {
            LOGE(TAG, "Error: cmd send fail!\r\n");
            return -1;
        }
        size += msgptr->cmdlen;
    } else {
        LOGD(TAG, "at going to send %s! datelen %d\n", (char *)msgptr->cmdptr,
             msgptr->datalen);

        ret = at.send_data_3stage_no_rsp((const char *)msgptr->cmdptr,
                                         msgptr->dataptr,
                                         msgptr->datalen, NULL);
        if (ret != 0) {
            LOGE(TAG, "Error: cmd and data send fail!\r\n");
            return -1;
        }
        size += (msgptr->cmdlen + msgptr->datalen);
    }

    return size;
}

// all uart send should go through this task
void uart_send_task()
{
    int ret;
    uint32_t size, sent_size;
    uart_send_info_t msg;

    LOG("uart send task start!\r\n");

    while ( true ) {
        if (!inited) {
            goto exit;
        }

        if (!aos_queue_is_valid(&uart_send_queue)) {
            LOGE(TAG, "Error uart send queue invalid!");
            goto exit;
        }

        memset(&msg, 0, sizeof(uart_send_info_t));
        ret = aos_queue_recv(&uart_send_queue, AOS_WAIT_FOREVER, &msg, &size);
        if (ret != 0) {
            LOGE(TAG, "Error uart send queue recv, errno %d, total fetch error %d\r\n", ret,
                 ++uart_send_statistic.fetch_error);
            goto done;
        }

        if (size != sizeof(uart_send_info_t)) {
            LOGE(TAG, "Error uart send recv: msg size %d is not valid\r\n", size);
            goto done;
        }

        if ((sent_size = send_over_uart(&msg)) < 0) {
            LOGE(TAG, "Error uart send fail, total send error %d\t\n", ++uart_send_statistic.send_error);
            goto done;
        }

done:

        if (uart_send_statistic.total_byte >= (msg.datalen + msg.cmdlen)) {
            uart_send_statistic.total_byte -= (msg.datalen + msg.cmdlen);
            LOGD(TAG, "uart send queue remain size %d \r\n",
                 uart_send_statistic.total_byte);
        } else {
            LOGE(TAG, "Error: uart send queue remain %d sent %d \r\n",
                 uart_send_statistic.total_byte, msg.datalen + msg.cmdlen);

            uart_send_statistic.total_byte = 0;
        }

        free_uart_send_msg(&msg);
    }

exit:
    LOG("Uart send task exits!\r\n");
    aos_task_exit(0);
}

static int sock_send_queue_init()
{
    uint32_t size = sizeof(sock_send_info_t) * DEFAULT_SOCK_SEND_BUF_SIZE;
    sock_send_info_t *sock_send_buf = NULL;

    memset(&sock_send_statistic, 0, sizeof(sock_send_statistic));

    sock_send_buf = (sock_send_info_t *) aos_malloc(size);
    if (!sock_send_buf) {
        LOGE(TAG, "sock send buf allocate %u fail!\r\n", size);
        goto err;
    }

    if (aos_queue_new(&sock_send_queue, sock_send_buf, size, sizeof(sock_send_info_t)) != 0) {
        LOGE(TAG, "sock send queue create fail!\r\n");
        goto err;
    }

    return 0;
err:
    aos_free(sock_send_buf);

    aos_queue_free(&sock_send_queue);

    return -1;
}

static int sock_send_queue_finalize()
{
    sock_send_info_t *sock_send_buf = NULL;

    if (!aos_queue_is_valid(&sock_send_queue)) {
        return -1;
    }

    sock_send_buf = (sock_send_info_t *) aos_queue_buf_ptr(&sock_send_queue);
    aos_free(sock_send_buf);

    aos_queue_free(&sock_send_queue);

    return 0;
}

void free_sock_send_msg(sock_send_info_t *msgptr)
{
    if (!msgptr) {
        return;
    }

    aos_free(msgptr->dataptr);
}

int insert_sock_send_msg(int sockfd, uint8_t *dataptr, uint16_t datalen)
{
    sock_send_info_t sock_send_buf;

    if (sockfd < 0 || !dataptr || !datalen) {
        return -1;
    }

    if (!aos_queue_is_valid(&sock_send_queue)) {
        return -1;
    }

    memset(&sock_send_buf, 0, sizeof(sock_send_info_t));
    sock_send_buf.dataptr = (uint8_t *) aos_malloc(datalen);
    if (!sock_send_buf.dataptr) {
        LOGE(TAG, "Sock send msg allocate fail\n");
        goto err;
    }

    memcpy(sock_send_buf.dataptr, dataptr, datalen);
    sock_send_buf.datalen = datalen;

    if (aos_queue_send(&sock_send_queue, &sock_send_buf, sizeof(sock_send_buf)) != 0) {
        LOGE(TAG, "Error: sock queue send fail, total fail %d!\r\n", ++sock_send_statistic.put_error);
        goto err;
    }

    sock_send_statistic.total_byte += datalen;
    LOGD(TAG, "insert sock send data datalen %d total %d\n", datalen,
         sock_send_statistic.total_byte);

    return 0;

err:
    free_sock_send_msg(&sock_send_buf);
    return -1;
}

// return total byte sent
int send_over_sock(sock_send_info_t *msgptr)
{
    int ret;
    int size = 0;

    if (!msgptr || msgptr->sockfd < 0 ||
        !msgptr->dataptr || !msgptr->datalen) {
        LOGE(TAG, "invalid sock data parameter!\n");
        return -1;
    }

    LOG("socket %d going to send data len %d!\n", msgptr->sockfd, msgptr->datalen);

    if (send(msgptr->sockfd, msgptr->dataptr, msgptr->datalen, 0) <= 0) {
        LOGE(TAG, "send data failed, errno = %d. \r\n", errno);
        return -1;
    }


    LOGD(TAG, "socket %d going to send data len %d!\n", msgptr->sockfd,
         msgptr->datalen);

    if (type == UDP_BROADCAST) {
        remotelen = sizeof(remote);
        if (sendto(msgptr->sockfd, msgptr->dataptr, msgptr->datalen, 0,
                   (struct sockaddr *)&remote, remotelen) <= 0) {
            LOGE(TAG,
                 "udp broadcast sock %d send data failed, errno = %d. \r\n",
                 msgptr->sockfd, errno);
            return -1;
        }
    } else {
        if (send(msgptr->sockfd, msgptr->dataptr, msgptr->datalen, 0) <= 0) {
            LOGE(TAG, "sock %d send data failed, errno = %d. \r\n",
                 msgptr->sockfd, errno);
            return -1;
        }
    }

    size += msgptr->datalen;

    return size;
}

// all socket data send should go through this task
void socket_send_task()
{
    int ret;
    uint32_t size, sent_size;
    sock_send_info_t msg;

    LOG("Socket send task starts!\r\n");

    while ( true ) {
        if (!inited) {
            LOGE(TAG, "at host not inited!\r\n");
            goto exit;
        }

        if (!aos_queue_is_valid(&sock_send_queue)) {
            LOGE(TAG, "Error sock send queue invalid!");
            goto exit;
        }

        memset(&msg, 0, sizeof(sock_send_info_t));
        ret = aos_queue_recv(&sock_send_queue, AOS_WAIT_FOREVER, &msg, &size);
        if (ret != 0) {
            LOGE(TAG, "Error sock send queue recv, errno %d, total fetch error %d\r\n", ret,
                 ++sock_send_statistic.fetch_error);
            goto done;
        }

        if (size != sizeof(sock_send_info_t)) {
            LOGE(TAG, "Error sock send recv: msg size %d is not valid\r\n", size);
            goto done;
        }

        if ((sent_size = send_over_sock(&msg)) <= 0) {
            LOGE(TAG, "Error sock send fail, errno %d, total fail %d\n", ++sock_send_statistic.send_error, errno);
            goto done;
        }

done:
        if (sent_size > 0 && sent_size != msg.datalen) {
            LOGE(TAG, "Erro send %d datalen %d\n", sent_size, msg.datalen);
        }

        if (sock_send_statistic.total_byte >= msg.datalen) {
            sock_send_statistic.total_byte -= msg.datalen;
            LOGD(TAG, "sock send queue remain size %d \r\n",
                 sock_send_statistic.total_byte);
        } else {
            LOGE(TAG, "Error: sock send queue remain %d sent %d \r\n",
                 sock_send_statistic.total_byte, sent_size);

            sock_send_statistic.total_byte = 0;
        }

        free_sock_send_msg(&msg);
    }

exit:
    LOG("Socket send task exits!\r\n");
    aos_task_exit(0);
}

void send_socket_data_task(void *arg)
{
    sock_send_info_t *sendarg;

    if (!arg) {
        goto exit;
    }

    sendarg = (struct socket_data_arg *) arg;

    if (sendarg->sockfd < 0   ||
        sendarg->dataptr == NULL ||
        sendarg->datalen <= 0) {
        LOGE(TAG, "invalid socket %d data len %d\n", sendarg->sockfd, sendarg->datalen);
        goto exit;
    }

    LOGD(TAG, "socket %d going to send data len %d!\n", sendarg->sockfd,
         sendarg->datalen);

    if (send(sendarg->sockfd, sendarg->dataptr, sendarg->datalen, 0) <= 0) {
        LOGE(TAG, "send data failed, errno = %d. \r\n", errno);
    }

exit:
    aos_free(sendarg->dataptr);
    aos_free(arg);
    aos_task_exit(0);
}

static int post_send_socket_data_task(int sockid, const char *data, int datalen)
{
    int size = sizeof(sock_send_info_t);
    sock_send_info_t *arg = NULL;
    char *buf = NULL;

    if (sockid < 0 || data == NULL || datalen <= 0) {
        LOGE(TAG, "invalid socket %d data len %d\n", sockid, datalen);
        goto exit;
    }

    arg = (sock_send_info_t *) aos_malloc(size);
    if (arg == NULL) {
        LOGE(TAG, "Fail to allcate memory %d byte for socket send task arg\r\n", size);
        goto exit;
    }

    size = datalen;
    buf = (char *) aos_malloc(size);
    if (buf == NULL) {
        LOGE(TAG, "Fail to allcate memory %d byte for socket send task buf\r\n", size);
        goto exit;
    }
    memcpy(buf, data, datalen);

    arg->sockfd = sockid;
    arg->dataptr = buf;
    arg->datalen = datalen;

    if (aos_task_new("socket_send_task", send_socket_data_task,
                     (void *) arg, 1024) != 0) {
        LOGE(TAG, "Fail to create socket send task\r\n");
        goto exit;
    }

    return 0;

exit:
    aos_free(buf);
    aos_free(arg);

    return -1;
}

void send_at_uart_task(void *arg)
{
    if (!arg) {
        goto exit;
    }

    LOGD(TAG, "at going to send %s!\n", (char *)arg);

    at.send_raw_no_rsp((char *)arg);
exit:
    aos_free(arg);
    aos_task_exit(0);
}

static int post_send_at_uart_task(const char *cmd)
{
    int size = strlen(cmd) + 1;
    char *tskarg = NULL;

    tskarg = (char *) aos_malloc(size);
    if (tskarg == NULL) {
        LOGE(TAG, "Fail to allcate memory %d byte for uart send task arg\r\n", size);
        goto exit;
    }
    memcpy(tskarg, cmd, size);

    if (aos_task_new("uart_send_task", send_at_uart_task,
                     (void *) tskarg, 1024) != 0) {
        LOGE(TAG, "Fail to create uart send task\r\n");
        goto exit;
    }

    return 0;

exit:
    aos_free(tskarg);
    return -1;
}

static int socket_data_len_check(char data)
{
    if (data > '9' || data < '0') {
        return -1;
    }
    return 0;
}

static int socket_ip_info_check(char data)
{
    if ((data > '9' || data < '0') && data != '.') {
        return -1;
    }

    return 0;
}

static int socket_conntype_check(char data)
{
    if ((data > 'z' || data < 'a') && data != '_') {
        return -1;
    }

    return 0;
}

void reverse(char s[])
{
    int i, j;
    char c;

    for (i = 0, j = strlen(s) - 1; i < j; i++, j--) {
        c = s[i];
        s[i] = s[j];
        s[j] = c;
    }
}

void itoa_decimal(int n, char s[])
{
    int i, sign;

    if ((sign = n) < 0) {
        n = -n;    /* make n positive */
    }
    i = 0;
    do {                         /* generate digits in reverse order */
        s[i++] = n % 10 + '0';   /* get next digit */
    } while ((n /= 10) > 0);     /* delete it */
    if (sign < 0) {
        s[i++] = '-';
    }
    s[i] = '\0';
    reverse(s);
}

// ret: -1 error, 0 more field, 1 no more field
static int socket_data_info_get(char *buf, uint32_t buflen, at_data_check_cb_t valuecheck)
{
    uint32_t i = 0;

    if (NULL == buf || 0 == buflen) {
        return -1;
    }

    do {
        at.parse(&buf[i], 1);
        if (buf[i] == ',') {
            buf[i] = 0;
            break;
        } else if (buf[i] == '\r') {
            LOGD(TAG, "********delimiter find here********\n");
            buf[i] = 0;
            return 1;
        }

        if (i >= buflen) {
            LOGE(TAG, "Too long length of data.reader is %s \r\n", buf);
            return -1;
        }
        if (NULL != valuecheck) {
            if (valuecheck(buf[i])) {
                LOGE(TAG, "Invalid string!!!, reader is %s last char %d\r\n", buf, buf[i]);
                return -1;
            }
        }
        i++;
    } while (1);

    return 0;
}

static int get_conntype_index(char *str)
{
    int i;

    if (NULL == str) {
        return -1;
    }

    for (i = 0; i < CONN_TYPE_NUM; i++) {
        if (memcmp(str, conntype_str[i], strlen(str)) == 0) {
            return i;
        }
    }

    return -1;
}

static int find_linkid_by_sockfd(int fd)
{
    int i;
    int linkid = -1;

    if (fd < 0) {
        return -1;
    }

    aos_mutex_lock(&g_link_mutex, AOS_WAIT_FOREVER);
    for (i = 0 ; i < LINK_ID_MAX; i++) {
        if (g_link[i].fd == fd) {
            linkid = g_link[i].linkid;
        }
    }
    aos_mutex_unlock(&g_link_mutex);

    return linkid;
}

static int find_conntype_by_sockfd(int fd)
{
    int i;
    int type;

    if (fd < 0) {
        return -1;
    }

    aos_mutex_lock(&g_link_mutex, AOS_WAIT_FOREVER);
    for (i = 0 ; i < LINK_ID_MAX; i++) {
        if (g_link[i].fd == fd) {
            type = g_link[i].type;
        }
    }
    aos_mutex_unlock(&g_link_mutex);

    return type;
}

static int find_sockfd_by_linkid(int linkid)
{
    int i;
    int fd = -1;

    if (linkid < 0) {
        return -1;
    }

    aos_mutex_lock(&g_link_mutex, AOS_WAIT_FOREVER);
    for (i = 0 ; i < LINK_ID_MAX; i++) {
        if (g_link[i].fd >= 0 &&
            g_link[i].linkid == linkid) {
            fd = g_link[i].fd;
            break;
        }
    }
    aos_mutex_unlock(&g_link_mutex);

    return fd;
}

static int add_link_info(int fd, int linkid, CONN_TYPE type)
{
    int i;
    int ret = -1;

    if (aos_mutex_lock(&g_link_mutex, AOS_WAIT_FOREVER) != 0) {
        LOGE(TAG, "Failed to lock mutex (%s).", __func__);
        return ret;
    }

    for (i = 0; i < LINK_ID_MAX; i++) {
        if (g_link[i].fd >= 0) {
            continue;
        } else {
            g_link[i].fd = fd;
            g_link[i].type = type;
            g_link[i].linkid = linkid;

            if (aos_sem_new(&g_link[i].sem_start, 0) != 0) {
                LOGE(TAG, "failed to allocate semaphore %s", __func__);
                g_link[i].fd = -1;
                g_link[i].linkid = -1;
                break;
            }

            if (aos_sem_new(&g_link[i].sem_close, 0) != 0) {
                LOGE(TAG, "failed to allocate semaphore %s", __func__);
                aos_sem_free(&g_link[i].sem_start);
                g_link[i].fd = -1;
                g_link[i].linkid = -1;
                break;
            }

            ret = 0;
            break;
        }
    }
    aos_mutex_unlock(&g_link_mutex);

    return ret;
}

static int delete_link_info_by_sockfd(int sockfd)
{
    int i;
    int ret = -1;

    if (sockfd < 0) {
        return ret;
    }

    if (aos_mutex_lock(&g_link_mutex, AOS_WAIT_FOREVER) != 0) {
        LOGE(TAG, "Failed to lock mutex (%s).", __func__);
        return ret;
    }

    for (i = 0; i < LINK_ID_MAX; i++) {
        if (g_link[i].fd == sockfd) {
            g_link[i].fd = -1;
            g_link[i].linkid = -1;

            if (aos_sem_is_valid(&g_link[i].sem_start)) {
                aos_sem_free(&g_link[i].sem_start);
            }

            if (aos_sem_is_valid(&g_link[i].sem_close)) {
                aos_sem_free(&g_link[i].sem_close);
            }

            ret = 0;
        }
    }
    aos_mutex_unlock(&g_link_mutex);

    return ret;
}

#define MAX_ATCMD_DATA_RECV_PREFIX_LEN 60
/*
 *  Network data recv event handler. Events includes:
 *   1. +CIPEVENT:SOCKET,id,len,data
 *   2. +CIPEVENT:UDP_BROADCAST,ip,port,id,len,data
 *
 *   data len should be within a reasonable range
 */
static int notify_cip_data_recv_event_unblock(int sockid, char *databuf, int datalen)
{
    char *type_str;
    char addr_str[16] = {0};  // ipv4 only
    int port;
    char port_str[10] = {0};
    char linkid_str[10] = {0};
    char datalen_str[10] = {0};
    char *sendbuf = NULL;
    int sendbuflen, offset = 0;
    int type, linkid;

    if (sockid < 0) {
        LOGE("Invalid sock id %d!\n", sockid);
        goto err;
    }

    // add one more for debug
    sendbuflen = MAX_ATCMD_DATA_RECV_PREFIX_LEN + datalen + 1 + 1;
    sendbuf = (char *) aos_malloc(sendbuflen);
    if (!sendbuf) {
        LOGE(TAG, "Error: %s %d out of memory, len is %d. \r\n",
             __func__, __LINE__, sendbuflen);
        goto err;
    }

    type = find_conntype_by_sockfd(sockid);
    if (type == UDP_BROADCAST) {
        type_str = "UDP_BROADCAST";
    } else {
        type_str = "SOCKET";
    }

    if (type == UDP_BROADCAST) {
        struct sockaddr_in peer;
        uint32_t peerlen = sizeof(struct sockaddr_in);
        char *remoteip;

        if (getpeername(sockid, (struct sockaddr *)&peer, &peerlen) != 0) {
            LOGE("Fail to sock %d get remote address!\n", sockid);
            goto err;
        }

        remoteip = inet_ntoa(peer.sin_addr);
        memcpy(addr_str, remoteip, strlen(remoteip));
        port = peer.sin_port;
    }

    if ((linkid = find_linkid_by_sockfd(sockid)) < 0) {
        LOGE(TAG, "Invalid link id %d!\n", linkid);
        goto err;
    }

    // prefix
    if (offset + strlen(prefix_cipevent) < sendbuflen) {
        offset += snprintf(sendbuf + offset, sendbuflen, "%s", prefix_cipevent);
    } else {
        LOGE(TAG, "at string too long %s\n", sendbuf);
        goto err;
    }

    // type
    if (offset + strlen(type_str) + 1 < sendbuflen) {
        offset += snprintf(sendbuf + offset, sendbuflen - offset, "%s,", type_str);
    } else {
        LOGE(TAG, "at string too long %s\n", sendbuf);
        goto err;
    }

    if (type == UDP_BROADCAST) {
        // ip
        if (offset + strlen(addr_str) + 1 < sendbuflen) {
            offset += snprintf(sendbuf + offset, sendbuflen - offset,
                               "%s,", addr_str);
        } else {
            LOGE(TAG, "at string too long %s\n", sendbuf);
            goto err;
        }

        // port
        itoa_decimal(port, port_str);
        if (offset + strlen(port_str) + 1 < sendbuflen) {
            offset += snprintf(sendbuf + offset, sendbuflen - offset,
                               "%s,", port_str);
        } else {
            LOGE(TAG, "at string too long %s\n", sendbuf);
            goto err;
        }
    }

    itoa_decimal(linkid, linkid_str);
    // append id
    if (offset + strlen(linkid_str) + 1 < sendbuflen) {
        offset += snprintf(sendbuf + offset, sendbuflen - offset,
                           "%s,", linkid_str);
    } else {
        LOGE(TAG, "at string too long %s\n", sendbuf);
        goto err;
    }

    itoa_decimal(datalen, datalen_str);
    // append datalen
    if (offset + strlen(datalen_str) + 1 < sendbuflen) {
        offset += snprintf(sendbuf + offset, sendbuflen - offset,
                           "%s,", datalen_str);
    } else {
        LOGE(TAG, "at string too long %s\n", sendbuf);
        goto err;
    }

    // append data
    if (offset + datalen < sendbuflen) {
        memcpy(sendbuf + offset, databuf, datalen);
    } else {
        LOGE(TAG, "at string too long %s\n", sendbuf);
        goto err;
    }

    if (post_send_at_uart_task(sendbuf) != 0) {
        LOGE(TAG, "fail to send at cmd %s\n", sendbuf);
        goto err;
    }

    aos_free(sendbuf);
    return 0;

err:
    aos_free(sendbuf);
    return -1;
}

static int notify_cip_data_recv_event(int sockid, char *databuf, int datalen)
{
    char *type_str;
    char addr_str[16] = {0};  // ipv4 only
    int port;
    char port_str[10] = {0};
    char linkid_str[10] = {0};
    char datalen_str[10] = {0};
    char sendbuf[MAX_ATCMD_DATA_RECV_PREFIX_LEN] = {0};
    int sendbuflen = MAX_ATCMD_DATA_RECV_PREFIX_LEN, offset = 0;
    int type, linkid;

    if (sockid < 0) {
        LOGE("Invalid sock id %d!\n", sockid);
        goto err;
    }

    type = find_conntype_by_sockfd(sockid);
    if (type == UDP_BROADCAST) {
        type_str = "UDP_BROADCAST";
    } else {
        type_str = "SOCKET";
    }

    if (type == UDP_BROADCAST) {
        struct sockaddr_in peer;
        uint32_t peerlen = sizeof(struct sockaddr_in);
        char *remoteip;

        if (getpeername(sockid, (struct sockaddr *)&peer, &peerlen) != 0) {
            LOGE("Fail to sock %d get remote address!\n", sockid);
            goto err;
        }

        remoteip = inet_ntoa(peer.sin_addr);
        memcpy(addr_str, remoteip, strlen(remoteip));
        port = peer.sin_port;
    }

    if ((linkid = find_linkid_by_sockfd(sockid)) < 0) {
        LOGE(TAG, "Invalid link id %d!\n", linkid);
        goto err;
    }

    // prefix
    if (offset + strlen(prefix_cipevent) < sendbuflen) {
        offset += snprintf(sendbuf + offset, sendbuflen, "%s", prefix_cipevent);
    } else {
        LOGE(TAG, "%s %d at string too long %s\n", __func__, __LINE__, sendbuf);
        goto err;
    }

    // type
    if (offset + strlen(type_str) + 1 < sendbuflen) {
        offset += snprintf(sendbuf + offset, sendbuflen - offset, "%s,", type_str);
    } else {
        LOGE(TAG, "%s %d at string too long %s\n", __func__, __LINE__,  sendbuf);
        goto err;
    }

    if (type == UDP_BROADCAST) {
        // ip
        if (offset + strlen(addr_str) + 1 < sendbuflen) {
            offset += snprintf(sendbuf + offset, sendbuflen - offset,
                               "%s,", addr_str);
        } else {
            LOGE(TAG, "%s %d at string too long %s\n", __func__, __LINE__, sendbuf);
            goto err;
        }

        // port
        itoa_decimal(port, port_str);
        if (offset + strlen(port_str) + 1 < sendbuflen) {
            offset += snprintf(sendbuf + offset, sendbuflen - offset,
                               "%s,", port_str);
        } else {
            LOGE(TAG, "%s %d at string too long %s\n", __func__, __LINE__, sendbuf);
            goto err;
        }
    }

    itoa_decimal(linkid, linkid_str);
    // append id
    if (offset + strlen(linkid_str) + 1 < sendbuflen) {
        offset += snprintf(sendbuf + offset, sendbuflen - offset,
                           "%s,", linkid_str);
    } else {
        LOGE(TAG, "%s %d at string too long %s\n", __func__, __LINE__, sendbuf);
        goto err;
    }

    itoa_decimal(datalen, datalen_str);
    // append datalen
    if (offset + strlen(datalen_str) + 1 < sendbuflen) {
        offset += snprintf(sendbuf + offset, sendbuflen - offset,
                           "%s,", datalen_str);
    } else {
        LOGE(TAG, "%s %d at string too long %s\n",  __func__, __LINE__, sendbuf);
        goto err;
    }

    if (insert_uart_send_msg(sendbuf, databuf, strlen(sendbuf), datalen) != 0) {
        LOGE(TAG, "Error insert uart send msg fail\r\n");
        goto err;
    }

    return 0;

err:
    return -1;
}

// TODO: add udp client
void tcp_client_recv_task(void *arg)
{
    char *             buf = NULL;
    int                len = 0;
    int                fd  = *((int *)arg);
    fd_set             readfds;
    int                remoteaddrlen;
    struct sockaddr_in remoteaddr;

    aos_free(arg);

    buf = (char *)aos_malloc(MAX_RECV_BUF_SIZE);
    if (NULL == buf) {
        LOGE("fail to malloc memory %d at %s %d \r\n", MAX_RECV_BUF_SIZE,
             __FUNCTION__, __LINE__);
        goto exit;
    }

    LOG("New udp broadcast task starts on socket %d\n", fd);

    while (1) {
        if (find_linkid_by_sockfd(fd) < 0) {
            LOGD("Client exit on socket %d\n", fd);
            goto exit;
        }

        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);

        if (select(fd + 1, &readfds, NULL, NULL, NULL) < 0) {
            LOGE(TAG, "Select fail! Client task exit!");
            goto exit;
        }

        if (FD_ISSET(fd, &readfds)) {
            memset(&remoteaddr, 0, sizeof(remoteaddr));
            len = recvfrom(fd, buf, MAX_RECV_BUF_SIZE, 0,
                           (struct sockaddr *)&remoteaddr, &remoteaddrlen);

            if (0 == len) {
                LOG("Client task (fd = %d) exit normally! ret %d \n", fd, len);
                goto exit;
            } else if (len < 0) {
                // TODO for some errror connection should be held
                LOGE(TAG, "Client task (fd = %d) recv error! ret %d errno %d\n",
                     fd, len, errno);
                goto exit;
            }

            LOGD(TAG, "Client task (fd = %d) recv len %d\n", fd, len);
            notify_cip_data_recv_event(fd, buf, len, &remoteaddr);
        }
    }

exit:
    aos_free(buf);

    // need to close by task
    if (find_linkid_by_sockfd(fd) >= 0) {
        notify_cip_connect_status_events(fd, CIP_STATUS_CLOSED, 0);
        // delete_link_info_by_sockfd(fd);
    }

    close(fd);
    aos_task_exit(1);
}

void tcp_udp_client_recv_task(void *arg)
{
    char * buf = NULL;
    int    len = 0;
    int    fd  = *((int *)arg);
    fd_set readfds;

    aos_free(arg);

    buf = (char *) aos_malloc(MAX_RECV_BUF_SIZE);
    if (NULL == buf) {
        LOGE("fail to malloc memory %d at %s %d \r\n", MAX_RECV_BUF_SIZE, __FUNCTION__, __LINE__);
        goto exit;
    }

    LOG("New client task starts on socket %d\n", fd);

    while ( 1 ) {
        if (find_linkid_by_sockfd(fd) < 0) {
            LOGD("Client exit on socket %d\n", fd);
            goto exit;
        }

        FD_ZERO( &readfds );
        FD_SET( fd, &readfds );

        if (select(fd + 1, &readfds, NULL, NULL, NULL ) < 0) {
            LOGE(TAG, "Select fail! Client task exit!");
            goto exit;
        }

        if (FD_ISSET(fd, &readfds)) {
            len = recv(fd, buf, MAX_RECV_BUF_SIZE, 0);
            if (0 == len) {
                LOG("Client task (fd = %d) exit normally! ret %d \n", fd, len);
                goto exit;
            } else if (len < 0) {
                // TODO for some errror connection should be held
                LOGE(TAG, "Client task (fd = %d) recv error! ret %d errno %d\n", fd, len, errno);
                goto exit;
            }

            LOGD(TAG, "Client task (fd = %d) recv len %d\n", fd, len);
            notify_cip_data_recv_event(fd, buf, len, NULL);
        }
    }

exit:
    aos_free(buf);

    // need to close by task
    if (find_linkid_by_sockfd(fd) >= 0) {
        notify_cip_connect_status_events(fd, CIP_STATUS_CLOSED, 0);
        //delete_link_info_by_sockfd(fd);
    }

    close(fd);
    aos_task_exit(1);
}

#define MAX_ATCMD_RESPONSE_LEN 20
static int notify_atcmd_recv_status(int status)
{
    int offset = 0;
    char *status_str;
    char response[MAX_ATCMD_RESPONSE_LEN] = {0};

    // prefix
    if (offset + strlen(AT_RECV_PREFIX) < MAX_ATCMD_RESPONSE_LEN) {
        offset += snprintf(response + offset, MAX_ATCMD_RESPONSE_LEN - offset, "%s", AT_RECV_PREFIX);
    } else {
        LOGE(TAG, "at string too long %s\n", response);
        goto err;
    }

    if (status == ATCMD_FAIL) {
        status_str = AT_RECV_FAIL_POSTFIX;
    } else if (status == ATCMD_SUCCESS) {
        status_str = AT_RECV_SUCCESS_POSTFIX;
    } else {
        LOGE(TAG, "unknown status\n", response);
        goto err;
    }

    // status
    if (offset + strlen(status_str) < MAX_ATCMD_RESPONSE_LEN) {
        offset += snprintf(response + offset, MAX_ATCMD_RESPONSE_LEN - offset, "%s", status_str);
    } else {
        LOGE(TAG, "at string too long %s\n", response);
        goto err;
    }

    if (insert_uart_send_msg(response, NULL, strlen(response), 0) != 0) {
        LOGE(TAG, "Error insert uart send msg fail\r\n");
        goto err;
    }

    return 0;
err:
    return -1;
}

/**
 * Network connection state event handler. Events includes:
 *   1. +CIPEVENT:id,SERVER,CONNECTED
 *   2. +CIPEVENT:id,SERVER,CLOSED
 *   3. +CIPEVENT:CLIENT,CONNECTED,ip,port
 *   4. +CIPEVENT:CLIENT,CLOSED,ip,port
 *   5. +CIPEVENT:id,UDP,CONNECTED
 *   6. +CIPEVENT:id,UDP,CLOSED
 */
#define MAX_ATCMD_CON_STATUS_LEN 80
static int notify_cip_connect_status_events(int sockid, int status, int recvstatus)
{
    char *status_str;
    char *type_str;
    char addr_str[16] = {0};  // ipv4 only
    int port;
    char port_str[6] = {0};
    char cmd[MAX_ATCMD_CON_STATUS_LEN] = {0};
    int offset = 0;
    int type, linkid;

    if (sockid < 0) {
        LOGE("Invalid sock id %d!\n", sockid);
        goto err;
    }

    if (status == CIP_STATUS_CONNECTED) {
        status_str = "CONNECTED";
    } else if (status == CIP_STATUS_CLOSED) {
        status_str = "CLOSED";
    } else {
        LOGE("Invalid connect status %d!\n", status);
        goto err;
    }

    type = find_conntype_by_sockfd(sockid);
    if (type == TCP_CLIENT || type == SSL_CLIENT) {
        type_str = "SERVER";
    } else if (type == TCP_SERVER) {
        struct sockaddr_in peer;
        uint32_t peerlen = sizeof(struct sockaddr_in);
        char *remoteip;
        type_str = "CLIENT";

        if (getpeername(sockid, (struct sockaddr *)&peer, &peerlen) != 0) {
            LOGE("Fail to sock %d get remote address!\n", sockid);
            goto err;
        }

        remoteip = inet_ntoa(peer.sin_addr);
        //TODO: check len
        memcpy(addr_str, remoteip, strlen(remoteip));
        port = peer.sin_port;
    } else if (type == UDP_BROADCAST || type == UDP_UNICAST) {
        type_str = "UDP";

    } else {
        LOGE(TAG, "Invalid connect type %d!\n", type);
        goto err;
    }

    if ((linkid = find_linkid_by_sockfd(sockid)) < 0) {
        LOGE(TAG, "Invalid link id %d!\n", linkid);
        goto err;
    }

    if (recvstatus > 0) {
        if (offset + strlen(AT_RECV_PREFIX) < MAX_ATCMD_CON_STATUS_LEN) {
            offset += snprintf(cmd + offset, MAX_ATCMD_CON_STATUS_LEN - offset, "%s", AT_RECV_PREFIX);
        } else {
            LOGE(TAG, "at string too long %s\n", cmd);
            goto err;
        }
    }

    // prefix
    if (offset + strlen(prefix_cipevent) < MAX_ATCMD_CON_STATUS_LEN) {
        offset += snprintf(cmd + offset, MAX_ATCMD_CON_STATUS_LEN - offset, "%s", prefix_cipevent);
    } else {
        LOGE(TAG, "at string too long %s\n", cmd);
        goto err;
    }

    if (type == TCP_SERVER) {
        // do nothing
    } else {
        char linkid_str[5] = {0};

        itoa_decimal(linkid, linkid_str);
        LOGD(TAG, "linkid %d linkid str -->%s<--\n", linkid, linkid_str);
        // append id
        if (offset + strlen(linkid_str) + 1 < MAX_ATCMD_CON_STATUS_LEN) {
            offset += snprintf(cmd + offset, MAX_ATCMD_CON_STATUS_LEN - offset,
                               "%s,", linkid_str);
        } else {
            LOGE(TAG, "at string too long %s\n", cmd);
            goto err;
        }
    }

    // type
    if (offset + strlen(type_str) + 1 < MAX_ATCMD_CON_STATUS_LEN) {
        offset += snprintf(cmd + offset, MAX_ATCMD_CON_STATUS_LEN - offset,
                           "%s,", type_str);
    } else {
        LOGE(TAG, "at string too long %s\n", cmd);
        goto err;
    }

    // status
    if (offset + strlen(status_str) < MAX_ATCMD_CON_STATUS_LEN) {
        offset +=  snprintf(cmd + offset, MAX_ATCMD_CON_STATUS_LEN - offset,
                            "%s", status_str);
    } else {
        LOGE(TAG, "at string too long %s\n", cmd);
        goto err;
    }

    if (type == TCP_SERVER) {
        // ip
        if (offset + strlen(addr_str) + 1 < MAX_ATCMD_CON_STATUS_LEN) {
            offset += snprintf(cmd + offset, MAX_ATCMD_CON_STATUS_LEN - offset,
                               ",%s", addr_str);
        } else {
            LOGE(TAG, "at string too long %s\n", cmd);
            goto err;
        }

        // port
        itoa_decimal(port, port_str);
        if (offset + strlen(port_str) + 1 + 1 < MAX_ATCMD_CON_STATUS_LEN) {
            offset += snprintf(cmd + offset, MAX_ATCMD_CON_STATUS_LEN - offset,
                               ",%s\r", port_str);
        } else {
            LOGE(TAG, "at string too long %s\n", cmd);
            goto err;
        }
    }

    if (recvstatus > 0) {
        if (offset + strlen(AT_RECV_SUCCESS_POSTFIX) < MAX_ATCMD_CON_STATUS_LEN) {
            offset += snprintf(cmd + offset, MAX_ATCMD_CON_STATUS_LEN - offset, "%s", AT_RECV_SUCCESS_POSTFIX);
        } else {
            LOGE(TAG, "at string too long %s\n", cmd);
            goto err;
        }
    }

    if (insert_uart_send_msg(cmd, NULL, strlen(cmd), 0) != 0) {
        LOGE(TAG, "Error insert uart send msg fail\r\n");
        goto err;
    }

    return 0;

err:
    return -1;
}

// AT+CIPSTART=linkid,conntype,address,remoteport
int atcmd_cip_start()
{
    char single;
    char body[16];
    int ret;
    int linkid;
    int type;
    char remoteip[16] = {0};
    uint16_t remoteport;
    struct sockaddr_in addr;
    int fd = -1;
    int socktype;
    recv_task_t recvtsk;
    char tskname[16] = {0};
    int *tskarg = NULL;
    int stacksize;

    if (!inited) {
        LOGE(TAG, "at host not inited yet!");
        goto err;
    }

    // Eat '='
    at.parse(&single, 1);
    if ('=' != single) {
        LOGE(TAG, "Invalid cip start prefix %c !", single);
        goto err;
    }

    // link id
    memset(body, 0, sizeof(body));
    ret = socket_data_info_get(body, sizeof(body), &socket_data_len_check);
    if (ret < 0 || (linkid = atoi(body)) < 0) {
        LOGE(TAG, "Invalid link id %s !!!\r\n", body);
        goto err;
    }

    // check linkid exist
    if (find_sockfd_by_linkid(linkid) >= 0) {
        LOGE(TAG, "link id %d exist !!!\r\n", linkid);
        goto err;
    }

    // connect type
    memset(body, 0, sizeof(body));
    ret = socket_data_info_get(body, sizeof(body), &socket_conntype_check);
    if (ret < 0 || (type = get_conntype_index(body)) < 0) {
        LOGE(TAG, "Invalid connect type %s !!!\r\n", body);
        goto err;
    }

    // remote ip
    ret = socket_data_info_get(remoteip, sizeof(remoteip), &socket_ip_info_check);
    if (ret < 0) {
        LOGE(TAG, "Invalid ip addr %s !!!\r\n", remoteip);
        goto err;
    }

    // port
    memset(body, 0, sizeof(body));
    ret = socket_data_info_get(body, sizeof(body), &socket_data_len_check);
    if (ret < 0) {
        LOGE(TAG, "Invalid portno %s !!!\r\n", body);
        goto err;
    }
    LOG("port %s\n", body);
    remoteport = atoi(body);

    memset(&addr, 0, sizeof(addr));
    addr.sin_port = htons(remoteport);
    if (0 == addr.sin_port) {
        LOGE(TAG, "invalid input port info %u \r\n", remoteport);
        goto err;
    }

    addr.sin_addr.s_addr = inet_addr(remoteip);
    if (IPADDR_NONE == addr.sin_addr.s_addr) {
        LOGE(TAG, "invalid input addr info %s \r\n", remoteip);
        goto err;
    }

    addr.sin_family = AF_INET;

    if (type == TCP_CLIENT ||
        type == SSL_CLIENT ||
        type == TCP_SERVER) {
        socktype = SOCK_STREAM;
    } else if (type == UDP_BROADCAST ||
               type == UDP_UNICAST) {
        socktype = SOCK_DGRAM;
    } else {
        LOGE(TAG, "invalid conntype %d \r\n", type);
        goto err;
    }

    fd = socket(AF_INET, socktype, 0);

    if (fd < 0) {
        LOGE(TAG, "fail to creat socket errno = %d \r\n", errno);
        goto err;
    }

    if (type == TCP_CLIENT) {
        char *prefix = "tcp_client";

        LOGD(TAG, "remote addr %u port %u \n", remoteaddr.sin_addr.s_addr,
             remoteport);
        if (connect(fd, (struct sockaddr *)&remoteaddr, sizeof(remoteaddr)) !=
            0) {
            LOGE(TAG, "TCP Connect failed, errno = %d, ip %s port %u \r\n",
                 errno, remoteip, remoteport);
            goto err;
        }
        LOGD(TAG, "TCP client connect success!\n");

        recvtsk = tcp_udp_client_recv_task;
        sprintf(tskname, "%s_%d", prefix, linkid);
        stacksize = 2048; // TODO need set by configuration
    } else if (type == UDP_UNICAST) {
        char *prefix = "udp_unicast";

        localaddr.sin_family      = AF_INET;
        localaddr.sin_addr.s_addr = htonl(INADDR_ANY);
        localaddr.sin_port        = htons(localport);

        if (bind(fd, (struct sockaddr *)&localaddr, sizeof(localaddr)) != 0) {
            LOGE(TAG,
                 "UDP unicast sock bind failed, errno = %d, local port %u \r\n",
                 errno, localport);
            goto err;
        }
        LOGD(TAG, "UDP unicast sock bind success!\n");

        LOG("addr %u port %u \n", addr.sin_addr.s_addr, remoteport);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            LOGE(TAG, "Connect failed, errno = %d, ip %s port %s \r\n", errno, remoteip, remoteport);
            goto err;
        }
        LOGD(TAG, "UDP unicast sock connect success!\n");

        recvtsk = tcp_udp_client_recv_task;
        sprintf(tskname, "%s_%d", prefix, linkid);
        stacksize = 1024; // TODO need set by configuration
    } else if (type == UDP_BROADCAST) {
        char *prefix    = "udp_broadcast";
        int   broadcast = 1;

        if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &broadcast,
                       sizeof(broadcast)) != 0) {
            LOGE(TAG, "setsockopt SO_BROADCAST fail, errno = %d \r\n", errno);
            goto err;
        }

        localaddr.sin_family      = AF_INET;
        localaddr.sin_addr.s_addr = htonl(INADDR_ANY);
        localaddr.sin_port        = htons(localport);

        if (bind(fd, (struct sockaddr *)&localaddr, sizeof(localaddr)) != 0) {
            LOGE(
              TAG,
              "UDP broadcast sock bind failed, errno = %d, local port %u \r\n",
              errno, localport);
            goto err;
        }
        LOGD(TAG, "UDP broadcast sock bind success!\n");

        /*if (connect(fd, (struct sockaddr *)&remoteaddr, sizeof(remoteaddr)) !=
        0) { LOGE(TAG, "UDP connect failed, errno = %d, ip %s port %u \r\n",
        errno, remoteip, remoteport); goto err;
        }
        LOGD(TAG, "UDP sock connect success!\n");*/

        update_remoteaddr_by_sockfd(fd, &remoteaddr);

        recvtsk = tcp_client_recv_task;
        sprintf(tskname, "%s_%d", prefix, linkid);
        stacksize = 2048; // TODO need set by configuration
    } else if (type == TCP_SERVER) {
        LOGW(TAG, "TCP server not implement yet!\n");
        goto err;
    }

    // save global info for new socket
    if (add_link_info(fd, linkid, type) != 0) {
        LOGE(TAG, "Fail to add link info for sock %d linkid %d type %d\r\n", fd, linkid, type);
        goto err;
    }

    if (aos_task_new(tskname, recvtsk, (void *) tskarg, stacksize) != 0) {
        LOGE(TAG, "Fail to create task %s\r\n", tskname);
        delete_link_info_by_sockfd(fd);
        goto err;
    }

    // notify over uart
    if (notify_cip_connect_status_events(fd, CIP_STATUS_CONNECTED, ATCMD_SUCCESS) != 0) {
        LOGE(TAG, "Fail to create task\r\n");
        delete_link_info_by_sockfd(fd);
        goto err;
    }

    return 0;

err:
    // notify fail response
    notify_atcmd_recv_status(ATCMD_FAIL);

    if (fd >= 0) {
        close(fd);
    }

    return -1;
}

// AT+CIPSEND=linkid,<remote_port>,datalen
int at_cip_send()
{
    char single;
    char body[16];
    char *recvdata = NULL, *tmp;
    int linkid, sockid;
    int remoteport, datalen;
    int ret;
    int readsize;

    if (!inited) {
        LOGE(TAG, "at host not inited yet!");
        goto err;
    }

    // Eat '='
    at.parse(&single, 1);
    if ('=' != single) {
        LOGE(TAG, "Invalid cip send prefix %c !", single);
        goto err;
    }

    // link id
    memset(body, 0, sizeof(body));
    ret = socket_data_info_get(body, sizeof(body), &socket_data_len_check);
    if (ret < 0 || (linkid = atoi(body)) < 0) {
        LOGE(TAG, "Invalid link id %s !!!\r\n", body);
        goto err;
    }

    // check linkid exist
    if ((sockid = find_sockfd_by_linkid(linkid)) < 0) {
        LOGE(TAG, "link id %d does not exist !!!\r\n", linkid);
        goto err;
    }

    // try get remote port
    memset(body, 0, sizeof(body));
    ret = socket_data_info_get(body, sizeof(body), &socket_data_len_check);
    if (ret < 0 || (remoteport = atoi(body)) < 0) {
        LOGE(TAG, "Invalid port %s !!!\r\n", body);
        goto err;
    }

    LOGD(TAG, "get remote port %d ret %d\n", remoteport, ret);
    if (ret == 0) {
        memset(body, 0, sizeof(body));
        ret = socket_data_info_get(body, sizeof(body), &socket_data_len_check);
        if (ret < 0 || (datalen = atoi(body)) < 0) {
            LOGE(TAG, "Invalid link id %s !!!\r\n", body);
            goto err;
        }
    } else if (ret == 1) {
        datalen = remoteport;
    }

    LOGD(TAG, "%s socket data on link %d with length %d to remote\n",
         __func__, linkid, datalen);

    // Prepare socket data
    recvdata = (char *)aos_malloc(datalen + 1);
    if (!recvdata) {
        LOGE(TAG, "Error: %s %d out of memory, len is %d. \r\n",
             __func__, __LINE__, datalen + 1);
        goto err;
    }

    if ((readsize = at.parse(recvdata, datalen)) <= 0) {
        LOGE(TAG, "Error at read data \r\n");
        goto err;
    }

    LOGD(TAG, "CIPSend datalen: %d readsize: %d\n", datalen, readsize);

    // TODO: what to do with remote port recvdata
    if (insert_sock_send_msg(sockid, recvdata, datalen) != 0) {
        LOGE(TAG, "Error insert send socket fail \r\n");
        goto err;
    }

    */

    sendpara.sockfd  = sockid;
    sendpara.dataptr = recvdata;
    sendpara.datalen = datalen;

    if (send_over_sock(&sendpara) <= 0) {
        LOGE(TAG, "Error send socket data fail \r\n");
        goto err;
    }*/

    notify_atcmd_recv_status(ATCMD_SUCCESS);
    aos_free(recvdata);
    return 0;

err:
    notify_atcmd_recv_status(ATCMD_FAIL);
    aos_free(recvdata);
    return -1;
}

// AT+CIPSTOP=linkid
int atcmd_cip_stop()
{
    char single;
    char body[5];
    int ret;
    int linkid, sockfd;

    if (!inited) {
        LOGE(TAG, "at host not inited yet!");
        goto err;
    }

    // Eat '='
    at.parse(&single, 1);
    if ('=' != single) {
        LOGE(TAG, "Invalid cip start prefix %c !", single);
        goto err;
    }

    // link id
    memset(body, 0, sizeof(body));
    ret = socket_data_info_get(body, sizeof(body), &socket_data_len_check);
    if (ret < 0 || (linkid = atoi(body)) < 0) {
        LOGE(TAG, "Invalid link id %s !!!\r\n", body);
        goto err;
    }

    // check linkid exist
    if ((sockfd = find_sockfd_by_linkid(linkid)) < 0) {
        LOGE(TAG, "link id %d does not exist !!!\r\n", linkid);
        goto err;
    }

    notify_cip_connect_status_events(sockfd, CIP_STATUS_CLOSED, ATCMD_SUCCESS);
    delete_link_info_by_sockfd(sockfd);
    return 0;

err:
    notify_atcmd_recv_status(ATCMD_FAIL);
    return -1;
}

// TODO: need flash read / write
// AT+CIPAUTOCONN=linkid,0/1
int atcmd_cip_auto_connect()
{
    char single;
    char body[5];
    int ret;
    int linkid, sockfd;
    int auto_set;

    if (!inited) {
        LOGE(TAG, "at host not inited yet!");
        goto err;
    }

    // Eat '='
    at.parse(&single, 1);
    if ('=' != single) {
        LOGE(TAG, "Invalid cip start prefix %c !", single);
        goto err;
    }

    // link id
    memset(body, 0, sizeof(body));
    ret = socket_data_info_get(body, sizeof(body), &socket_data_len_check);
    if (ret < 0 || (linkid = atoi(body)) < 0) {
        LOGE(TAG, "Invalid link id %s !!!\r\n", body);
        goto err;
    }

    // set bit
    at.parse(&single, 1);
    if (single != '0' && single != '1') {
        LOGE(TAG, "Invalid auto connect set %c !!!\r\n", single);
        goto err;
    }
    auto_set = single - '0';

    // TODO: check linkid info exist from flash
    if ((sockfd = find_sockfd_by_linkid(linkid)) < 0) {
        LOGE(TAG, "link id %d does not exist !!!\r\n", linkid);
        goto err;
    }

    // close connection
    if (0 == auto_set) {
        notify_cip_connect_status_events(sockfd, CIP_STATUS_CLOSED, ATCMD_SUCCESS);
        delete_link_info_by_sockfd(sockfd);
    }

    // TODO: change the setting on the flash.

    return 0;

err:
    notify_atcmd_recv_status(ATCMD_FAIL);
    return -1;
}

#define MAX_ATCMD_DOMAIN_LEN 80
// AT+CIPDOMAIN=domain
// Respone: AT+CIPDOMAIN:180.97.33.108
int atcmd_cip_domain_dns()
{
    char single;
    char domain[50];
    char addr_str[16];  // ipv4 only
    char response[MAX_ATCMD_DOMAIN_LEN] = {0};
    int ret;
    struct hostent *host;
    struct in_addr **addrlist;
    int i, offset = 0;
    char *index;

    if (!inited) {
        LOGE(TAG, "at host not inited yet!");
        goto err;
    }

    // Eat '='
    at.parse(&single, 1);
    if ('=' != single) {
        LOGE(TAG, "Invalid cip start prefix %c !", single);
        goto err;
    }

    // domain
    memset(domain, 0, sizeof(domain));
    ret = socket_data_info_get(domain, sizeof(domain), NULL);
    if (ret < 0) {
        LOGE(TAG, "Invalid domain %s !!!\r\n", domain);
        goto err;
    }

    if ((host = gethostbyname(domain)) == NULL) {
        LOGE(TAG, "fail to find domain %s !!!\r\n", domain);
        goto err;
    }

    addrlist = (struct in_addr **) host->h_addr_list;
    for (i = 0; addrlist[i] != NULL; i++) {
        // return the first one
        strcpy(addr_str, inet_ntoa(*addrlist[i]));
        break;
    }

    // AT_RECV_PREFIX
    if (offset + strlen(AT_RECV_PREFIX) < MAX_ATCMD_DOMAIN_LEN) {
        offset += snprintf(response + offset, MAX_ATCMD_DOMAIN_LEN - offset,
                           "%s", AT_RECV_PREFIX);
    } else {
        LOGE(TAG, "at string too long %s\n", response);
        goto err;
    }

    if (offset + strlen(prefix_cipdomain) < MAX_ATCMD_DOMAIN_LEN) {
        offset += snprintf(response + offset, MAX_ATCMD_DOMAIN_LEN - offset,
                           "%s", prefix_cipdomain);
    } else {
        LOGE(TAG, "at string too long %s\n", response);
        goto err;
    }

    // default 0
    index = "0";
    if (offset + strlen(index) < MAX_ATCMD_DOMAIN_LEN) {
        offset += snprintf(response + offset, MAX_ATCMD_DOMAIN_LEN - offset,
                           "%s", index);
    } else {
        LOGE(TAG, "at string too long %s\n", response);
        goto err;
    }

    // AT_RECV_PREFIXf
    if (offset + strlen(AT_RECV_PREFIX) < MAX_ATCMD_DOMAIN_LEN) {
        offset += snprintf(response + offset, MAX_ATCMD_DOMAIN_LEN - offset,
                           "%s", AT_RECV_PREFIX);
    } else {
        LOGE(TAG, "at string too long %s\n", response);
        goto err;
    }

    if (offset + strlen(addr_str) < MAX_ATCMD_DOMAIN_LEN) {
        offset += snprintf(response + offset, MAX_ATCMD_DOMAIN_LEN - offset,
                           "%s", addr_str);
    } else {
        LOGE(TAG, "at string too long %s\n", response);
        goto err;
    }

    // AT_RECV_PREFIX
    if (offset + strlen(AT_RECV_PREFIX) < MAX_ATCMD_DOMAIN_LEN) {
        offset += snprintf(response + offset, MAX_ATCMD_DOMAIN_LEN - offset,
                           "%s", AT_RECV_PREFIX);
    } else {
        LOGE(TAG, "at string too long %s\n", response);
        goto err;
    }

    // AT_RECV_SUCCESS_POSTFIX
    if (offset + strlen(AT_RECV_SUCCESS_POSTFIX) < MAX_ATCMD_DOMAIN_LEN) {
        offset += snprintf(response + offset, MAX_ATCMD_DOMAIN_LEN - offset,
                           "%s", AT_RECV_SUCCESS_POSTFIX);
    } else {
        LOGE(TAG, "at string too long %s\n", response);
        goto err;
    }

    if (insert_uart_send_msg(response, NULL, strlen(response), 0) != 0) {
        LOGE(TAG, "%s %d insert uart send msg fail\r\n", __func__, __LINE__);
        goto err;
    }

    return 0;
err:
    notify_atcmd_recv_status(ATCMD_FAIL);
    return -1;
}

/*
 * Wifi station event handler. include:
 *  +WEVENT:AP_UP
 *  +WEVENT:AP_DOWN
 *  +WEVENT:STATION_UP
 *  +WEVENT:STATION_DOWN
 */
#define MAX_ATCMD_AP_STA_STATUS_LEN 30
int notify_AP_STA_status_events(int type, int status)
{
    char *status_str;
    char *type_str;
    char cmd[MAX_ATCMD_AP_STA_STATUS_LEN] = {0};
    int offset = 0;

    if (type == AP) {
        type_str = "AP_";
    } else if (type == STA) {
        type_str = "STATION_";
    } else {
        LOGE("Invalid type %d!\n", type);
        goto err;
    }

    if (status == WEVENT_STATUS_UP) {
        status_str = "UP";
    } else if (status == WEVENT_STATUS_DOWN) {
        status_str = "DOWN";
    } else {
        LOGE("Invalid connect status %d!\n", status);
        goto err;
    }

    if (offset + strlen(prefix_wevent) < MAX_ATCMD_AP_STA_STATUS_LEN) {
        offset += snprintf(cmd + offset, MAX_ATCMD_AP_STA_STATUS_LEN - offset,
                           "\r\n%s", prefix_wevent);
    } else {
        LOGE(TAG, "at string too long %s\n", cmd);
        goto err;
    }

    if (offset + strlen(type_str) + strlen(status_str) <
        MAX_ATCMD_AP_STA_STATUS_LEN) {
        offset += snprintf(cmd + offset, MAX_ATCMD_AP_STA_STATUS_LEN - offset,
                           "%s%s\r\n", type_str, status_str);
    } else {
        LOGE(TAG, "at string too long %s\n", cmd);
        goto err;
    }

    if (insert_uart_send_msg(cmd, NULL, strlen(cmd), 0) != 0) {
        LOGE(TAG, "%s %d post send at uart task fail!\n", __func__, __LINE__);
        goto err;
    }

    return 0;

err:
    return -1;
}

static void ip_got_event(hal_wifi_module_t *m,
                         hal_wifi_ip_stat_t *pnet,
                         void *arg)
{
    LOGD(TAG, "%s - ip: %s, gw: %s, mask: %s", __func__, pnet->ip, pnet->gate,
        pnet->mask);
    /*if (aos_sem_is_valid(&start_sem)) {
        aos_sem_signal(&start_sem);
    }*/
    ip_ready = true;

    notify_AP_STA_status_events(STA, WEVENT_STATUS_UP);
}

static void stat_chg_event(hal_wifi_module_t *m,
                           hal_wifi_event_t stat,
                           void *arg)
{
    switch (stat) {
        case NOTIFY_STATION_UP:
            ip_ready = true;
            notify_AP_STA_status_events(STA, WEVENT_STATUS_UP);
            break;
        case NOTIFY_STATION_DOWN:
            ip_ready = false;
            notify_AP_STA_status_events(STA, WEVENT_STATUS_DOWN);
            break;
        case NOTIFY_AP_UP:
            notify_AP_STA_status_events(AP, WEVENT_STATUS_UP);
            break;
        case NOTIFY_AP_DOWN:
            notify_AP_STA_status_events(AP, WEVENT_STATUS_DOWN);
            break;
        default:
            break;
    }
}

static int register_wifi_events()
{
    hal_wifi_module_t *m;

    m = hal_wifi_get_default_module();
    if (!m) {
        LOGE(TAG, "failed: no default wifi module.");
        return -1;
    }

    /* m->ev_cb is declared as const, can only be assigned once. */
    if (m->ev_cb == NULL) {
        m->ev_cb = &wifi_events;
    }

    return 0;
}

static int start_wifi(const char *ssid, const char *key)
{
    int ret = -1;
    hal_wifi_init_type_t type;

    if (!ssid || !key) {
        LOGE(TAG, "%s: invalid argument.", __func__);
        LOGE(TAG, "Starting wifi failed.");
        return -1;
    }

    if (register_wifi_events() != 0) {
        LOGE(TAG, "%s failed to register wifi events.", __func__);
        return -1;
    }
    wifi_events.ip_got = ip_got_event;
    wifi_events.stat_chg = stat_chg_event;


    /*ret = aos_sem_new(&start_sem, 0);
    if (0 != ret) {
        LOGE(TAG, "%s failed to allocate sem.", __func__);
        return;
    }*/

    memset(&type, 0, sizeof(type));
    type.wifi_mode = STATION;
    type.dhcp_mode = DHCP_CLIENT;
    strncpy(type.wifi_ssid, ssid, sizeof(type.wifi_ssid) - 1);
    strncpy(type.wifi_key, key, sizeof(type.wifi_key) - 1);
    ret = hal_wifi_start(NULL, &type);
    if (ret != 0) {
        LOGE(TAG, "%s failed to start hal wifi.", __func__);
        return -1;
    }

    LOGD(TAG, "Wifi started (ssid: %s, password: %s').", ssid, key);
    //aos_sem_wait(&start_sem, 60000);
    //aos_sem_free(&start_sem);
    return 0;
}

#define MAX_WIFI_SSID_LEN 32
#define MAX_WIFI_KEY_LEN 64
// AT+WJAP=ssid,key
int atcmd_ap_connect()
{
    char ssid[MAX_WIFI_SSID_LEN + 1] = {0};
    char key[MAX_WIFI_KEY_LEN + 1] = {0};
    int offset = 0;
    int ret;

    if (!inited) {
        LOGE(TAG, "at host not inited yet!");
        goto err;
    }

    // ssid
    ret = socket_data_info_get(ssid, sizeof(ssid), NULL);
    if (ret < 0) {
        LOGE(TAG, "Invalid ssid %s !!!\r\n", ssid);
        goto err;
    }

    // key
    ret = socket_data_info_get(key, sizeof(key), NULL);
    if (ret < 0) {
        LOGE(TAG, "Invalid key %s !!!\r\n", key);
        goto err;
    }

    ret = start_wifi(ssid, key);
    if (ret < 0) {
        LOGE(TAG, "Start wifi fail !!!\r\n");
        goto err;
    }

    notify_atcmd_recv_status(ATCMD_SUCCESS);
    return 0;

err:
    notify_atcmd_recv_status(ATCMD_FAIL);
    return -1;
}

#define MAX_WIFI_IPINFO_LEN 90
// AT+WJAPIP:<ip>,<msk>,<gateway>,<dns>
int atcmd_get_ip()
{
    char response[MAX_WIFI_IPINFO_LEN] = {0};
    hal_wifi_ip_stat_t ip_stat;
    int ret;
    int offset = 0;

    memset(&ip_stat, 0 , sizeof(ip_stat));
    ret = hal_wifi_get_ip_stat(NULL, &ip_stat, STATION);
    if (ret != 0) {
        LOGE(TAG, "%s get ip fail\r\n", __func__);
        goto err;
    }

    // AT_RECV_PREFIX
    if (offset + strlen(AT_RECV_PREFIX) < MAX_WIFI_IPINFO_LEN) {
        offset += snprintf(response + offset, MAX_WIFI_IPINFO_LEN - offset,
                           "%s", AT_RECV_PREFIX);
    } else {
        LOGE(TAG, "at string too long %s\n", response);
        goto err;
    }

    // WJAPIP prefix
    if (offset + strlen(prefix_wjapip) < MAX_WIFI_IPINFO_LEN) {
        offset += snprintf(response + offset, MAX_WIFI_IPINFO_LEN - offset,
                           "%s", prefix_wjapip);
    } else {
        LOGE(TAG, "at string too long %s\n", response);
        goto err;
    }

    // ip info
    if (offset + strlen(ip_stat.ip) * 4 + 4 < MAX_WIFI_IPINFO_LEN) {
        offset += snprintf(response + offset, MAX_WIFI_IPINFO_LEN - offset,
                           "%s,%s,%s,%s\r", ip_stat.ip, ip_stat.mask, ip_stat.gate, ip_stat.dns);
    } else {
        LOGE(TAG, "at string too long %s\n", response);
        goto err;
    }

    if (offset + strlen(AT_RECV_SUCCESS_POSTFIX) < MAX_WIFI_IPINFO_LEN) {
        offset += snprintf(response + offset, MAX_WIFI_IPINFO_LEN - offset,
                           "%s", AT_RECV_SUCCESS_POSTFIX);
    } else {
        LOGE(TAG, "at string too long %s\n", response);
        goto err;
    }

    if (insert_uart_send_msg(response, NULL, strlen(response), 0) != 0) {
        LOGE(TAG, "%s %d post send at uart task fail!\n", __func__, __LINE__);
        goto err;
    }

    return 0;

err:
    notify_atcmd_recv_status(ATCMD_FAIL);
    return -1;
}

#define MAX_WIFI_MACINFO_LEN 40
// AT+WMAC:<mac>
int atcmd_get_mac()
{
    char response[MAX_WIFI_MACINFO_LEN] = {0};
    hal_wifi_ip_stat_t ip_stat;
    int ret;
    int offset = 0;

    ret = hal_wifi_get_ip_stat(NULL, &ip_stat, STATION);
    if (ret != 0) {
        LOGE(TAG, "%s get ip fail\r\n", __func__);
        goto err;
    }

    // AT_RECV_PREFIX
    if (offset + strlen(AT_RECV_PREFIX) < MAX_WIFI_MACINFO_LEN) {
        offset += snprintf(response + offset, MAX_WIFI_MACINFO_LEN - offset,
                           "%s", AT_RECV_PREFIX);
    } else {
        LOGE(TAG, "at string too long %s\n", response);
        goto err;
    }

    // WJAPIP prefix
    if (offset + strlen(prefix_wmac) + 1 < MAX_WIFI_MACINFO_LEN) {
        offset += snprintf(response + offset, MAX_WIFI_MACINFO_LEN - offset,
                           "%s:", prefix_wmac);
    } else {
        LOGE(TAG, "at string too long %s\n", response);
        goto err;
    }

    // mac info
    if (offset + strlen(ip_stat.mac) < MAX_WIFI_MACINFO_LEN) {
        offset += snprintf(response + offset, MAX_WIFI_MACINFO_LEN - offset,
                           "%s\r", ip_stat.mac);
    } else {
        LOGE(TAG, "at string too long %s\n", response);
        goto err;
    }

    if (offset + strlen(AT_RECV_SUCCESS_POSTFIX) < MAX_WIFI_MACINFO_LEN) {
        offset += snprintf(response + offset, MAX_WIFI_MACINFO_LEN - offset,
                           "%s", AT_RECV_SUCCESS_POSTFIX);
    } else {
        LOGE(TAG, "at string too long %s\n", response);
        goto err;
    }

    if (insert_uart_send_msg(response, NULL, strlen(response), 0) != 0) {
        LOGE(TAG, "%s %d post send at uart task fail!\n", __func__, __LINE__);
        goto err;
    }

    return 0;

err:
    notify_atcmd_recv_status(ATCMD_FAIL);
    return -1;
}

// AT+UARTE=OFF
int atcmd_uart_echo()
{
    int ret;
    char single;
    char body[10] = {0};
    bool echo;

    if (!inited) {
        LOGE(TAG, "at host not inited yet!");
        goto err;
    }

    // Eat '='
    at.parse(&single, 1);
    if ('=' != single) {
        LOGE(TAG, "Invalid cip start prefix %c !", single);
        goto err;
    }

    // echo setting
    ret = socket_data_info_get(body, sizeof(body), NULL);
    if (ret < 0) {
        LOGE(TAG, "Invalid command %s !!!\r\n", body);
        goto err;
    }

    if (memcmp(body, "ON", strlen(body)) == 0) {
        uart_echo_on = true;
    } else if (memcmp(body, "OFF", strlen(body)) == 0) {
        uart_echo_on = false;
    } else {
        LOGE(TAG, "Invalid command %s !!!\r\n", body);
        goto err;
    }

    LOGD(TAG, "UART echo done!\n");
    notify_atcmd_recv_status(ATCMD_SUCCESS);

    return 0;

err:
    notify_atcmd_recv_status(ATCMD_FAIL);
    return -1;
}

#if 0
static void extract_frame_info(uint8_t *data, int len, frame_info_t *info)
{
    ieee80211_hdr_t *hdr = (ieee80211_hdr_t *)data;

    info->src = ieee80211_get_SA(hdr);
    info->dst = ieee80211_get_DA(hdr);
    info->bssid = ieee80211_get_BSSID(hdr);
}
#endif

#ifndef MONITOR_PKT_MAX_LEN
#define MONITOR_PKT_MAX_LEN 2000
#endif

/**
 * YWSS monitor AT data event:
 *    +YEVENT:rssi,len,data
 */
static void monitor_cb(uint8_t *data, int len, hal_wifi_link_info_t *info)
{
    char header[32] = {0};

    snprintf(header, 31, "+YEVENT:%d,%d,", info->rssi, len);
    if (len > MONITOR_PKT_MAX_LEN) {
        LOGI(TAG, "Packet length (%d) exceed limit (%d), will drop it.", len, MONITOR_PKT_MAX_LEN);
        return;
    }

    at.send_data_3stage_no_rsp(header, data, len, NULL);
}

static int at_ywss_start_monitor()
{
    LOGD(TAG, "hello %s\r\n", __func__);
    at.send_raw_no_rsp("\r\nOK\r\n");
    at.send_raw_no_rsp("\r\n+YEVENT:MONITOR_UP\r\n");
    aos_msleep(200);
    hal_wifi_register_monitor_cb(NULL, monitor_cb);
    hal_wifi_start_wifi_monitor(NULL);
}

static int at_ywss_stop_monitor()
{
    LOGD(TAG, "hello %s\r\n", __func__);
    at.send_raw_no_rsp("\r\nOK\r\n");
    hal_wifi_register_monitor_cb(NULL, NULL);
    hal_wifi_stop_wifi_monitor(NULL);
    at.send_raw_no_rsp("\r\n+YEVENT:MONITOR_DOWN\r\n");
}

static int at_ywss_set_channel()
{
    int ch = 0, doswitch = 0, i = 0;
    char c, *sdelmiter = AT_SEND_DELIMITER, tmp[sizeof(AT_SEND_DELIMITER)] = {0};

    LOGD(TAG, "hello %s entry\r\n", __func__);

    while (1) {
        at.parse(&c, 1);
        if (c == sdelmiter[0]) {
            if (strlen(AT_SEND_DELIMITER) > 1) {
                at.parse(tmp, strlen(AT_SEND_DELIMITER) - 1);
                if (memcmp(tmp, &sdelmiter[1], strlen(AT_SEND_DELIMITER) - 1) != 0) {
                    LOGE(TAG, "invalid string (%s) found in ywss set channel cmd", tmp);
                    break;
                }
            }
            doswitch = 1;
            break;
        }

        if (c > '9' || c < '0') {
            LOGE(TAG, "invalid channel number found (%c) in ywss set channel cmd", c);
            break;
        }

        ch = (ch << 3) + (ch << 1) + c - '0';
    }

    if (doswitch) {
        LOGD(TAG, "channel to switch to %d", ch);
        hal_wifi_set_channel(NULL, ch);
        at.send_raw_no_rsp("\r\nOK\r\n");
    } else {
        at.send_raw_no_rsp("\r\nERROR\r\n");
    }

    LOGD(TAG, "hello %s exit\r\n", __func__);
}

static int at_ywss_suspend_sta()
{
    int ret = hal_wifi_suspend_station(NULL);

    if (ret == 0) {
        at.send_raw_no_rsp("\r\nOK\r\n");
    } else {
        at.send_raw_no_rsp("\r\nERROR\r\n");
    }
}

enum {
    ATCMD_WJAP_CONN = 0,
    ATCMD_WJAP_IP,
    ATCMD_WJAP_MAC,
    ATCMD_UART_ECHO,
    ATCMD_CIP_DOMAIN,
    ATCMD_CIP_AUTOCONN,
    ATCMD_CIP_START,
    ATCMD_CIP_STOP,
    ATCMD_CIP_SEND,
    ATCMD_YWSS_START_MONITOR,
    ATCMD_YWSS_STOP_MONITOR,
    ATCMD_YWSS_SET_CHANNEL,
    ATCMD_YWSS_SUSPEND_STA,
};

static const struct at_cli_command at_cmds_table[] = {
    // wifi
    {.name = "AT+WJAP=", .help = "AT+WJAP=<ssid>,<key>", .function = atcmd_ap_connect},
    {.name = "AT+WJAPIP?", .help = "AT+WJAPIP?", .function = atcmd_get_ip},
    {.name = "AT+WMAC?", .help = "AT+WMAC?", .function = atcmd_get_mac},

    // uart setting
    {.name = "AT+UARTE", .help = "AT+UARTE=<ON/OFF>", .function = atcmd_uart_echo},

    // TCP/UDP:
    {.name = "AT+CIPDOMAIN", .help = "AT+CIPDOMAIN", .function = atcmd_cip_domain_dns},
    {.name = "AT+CIPAUTOCONN", .help = "AT+CIPAUTOCONN=<id>[,option]", .function = atcmd_cip_auto_connect},
    {.name = "AT+CIPSTART", .help = "AT+CIPSTART", .function = atcmd_cip_start},
    {.name = "AT+CIPSTOP", .help = "AT+CIPSTOP", .function = atcmd_cip_stop},
    {.name = "AT+CIPSEND", .help = "AT+CIPSEND=<id>,[<remote_port>,]<data_length>", .function = at_cip_send},

    // ywss
    {.name = "AT+YWSSSTARTMONITOR", .help = "AT+YWSSSTARTMONITOR", .function = at_ywss_start_monitor},
    {.name = "AT+YWSSSTOPMONITOR", .help = "AT+YWSSSTOPMONITOR", .function = at_ywss_stop_monitor},
    {.name = "AT+YWSSSETCHANNEL", .help = "AT+YWSSETCHANNEL", .function = at_ywss_set_channel},
    {.name = "AT+YWSSSUSPENDSTATION", .help = "AT+YWSSSUSPENDSTATION", .function = at_ywss_suspend_sta},
};

static int athost_init()
{
    int i;

    if (inited) {
        LOGW(TAG, "at host already initialized");
        return 0;
    }

    if (aos_mutex_new(&g_link_mutex) != 0) {
        LOGE(TAG, "Creating link mutex failed (%s %d).", __func__, __LINE__);
        goto err;
    }

    memset(g_link, 0, sizeof(g_link));
    for (i = 0; i < LINK_ID_MAX; i++) {
        g_link[i].fd = -1;
        g_link[i].linkid = -1;
    }

    if (uart_send_queue_init() != 0) {
        LOGE(TAG, "Creating uart send que fail (%s %d).", __func__, __LINE__);
        goto err;
    }

    if (aos_task_new("athost_uart_send_task", uart_send_task, NULL, 1024) !=
        0) {
        LOGE(TAG, "Fail to create uart send task\r\n");
        goto err;
    }

    if (sock_send_queue_init() != 0) {
        LOGE(TAG, "Creating sock send que fail (%s %d).", __func__, __LINE__);
        goto err;
    }

    /*
    if (aos_task_new("athost_socket_send_task", socket_send_task, NULL, 1024) !=
        0) {
        LOGE(TAG, "Fail to create socket send task\r\n");
        goto err;
    }
    */

    inited = true;
    return 0;

err:
    if (aos_mutex_is_valid(&g_link_mutex)) {
        aos_mutex_free(&g_link_mutex);
    }

    uart_send_queue_finalize();

    sock_send_queue_finalize();

    inited = false;

    return -1;
}

static int uart_echo()
{
    char buf[1024];
    char out[1024];
    char info[] = "MSG too long";

    int i = 0;

    do {
        if (!uart_echo_on) {
            return 0;
        }

        if (i >= sizeof(buf)) {
            LOGE(TAG, "Too long length\r\n");
            break;
        }

        if (at.parse(&buf[i], 1) <= 0) {
            LOGE(TAG, "read fail\r\n");
            break;
        }

        // end of message then echo
        if (buf[i] == '\r') {
            buf[i] = '\0';

            notify_atcmd_recv_status(ATCMD_SUCCESS);

            if (memcmp(buf, "UARTE=OFF", strlen(buf)) == 0) {
                uart_echo_on = false;
                break;
            }

            LOGD(TAG, "Echo server recv msg len %d\n", i);

            memcpy(out, buf, i);
            memcpy(buf, prefix_athost, strlen(prefix_athost));
            if (i + strlen(prefix_athost) + 1 < sizeof(buf)) {
                memcpy(buf + strlen(prefix_athost), out, i);
                buf[strlen(prefix_athost) + i ] = '\r';
                buf[strlen(prefix_athost) + i + 1] = '\0';
            } else {
                memcpy(buf + strlen(prefix_athost), info, strlen(info));
                buf[strlen(prefix_athost) + strlen(info)] = '\0';
            }

            at.send_raw_no_rsp(buf);
            break;
        }
        i++;
    } while (1);

    return 1;
}

static struct at_cli_command *get_atcmd_cip_handler()
{
    const char *cmd_prefix = "IP";
    char prefix[MAX_ATCMD_PREFIX] = {0};
    char single;
    int index = -1;

    // Eat "IP"
    at.parse(prefix, strlen(cmd_prefix));
    if (memcmp(prefix, cmd_prefix, strlen(cmd_prefix)) != 0) {
        LOGE(TAG, "invalid cip prefix %s\n", prefix);
        return NULL;
    }

    at.parse(&single, 1);

    switch (single) {
        case 'S':
            at.parse(prefix, 3);

            if (memcmp(prefix, "TAR", 3) == 0) {
                // Eat 'T'
                at.parse(&single, 1);
                index = ATCMD_CIP_START;
            } else if (memcmp(prefix, "TOP", 3) == 0) {
                index = ATCMD_CIP_STOP;
            } else if (memcmp(prefix, "END", 3) == 0) {
                index = ATCMD_CIP_SEND;
            } else {
                LOGE(TAG, "invalid cip prefix %s\n", prefix);
            }
            break;

        case 'D':
            // Eat "OMAIN"
            at.parse(prefix, 5);
            index = ATCMD_CIP_DOMAIN;
            break;

        case 'A':
            // Eat "UTOCONN"
            at.parse(prefix, 7);
            index = ATCMD_CIP_AUTOCONN;
            break;

        default:
            LOGE(TAG, "invalid cip prefix %c\n", single);
            break;
    }

    if (index >= 0 && index < sizeof(at_cmds_table)) {
        return &at_cmds_table[index];
    }

    return NULL;
}

static struct at_cli_command *get_atcmd_uart_handler()
{
    const char *cmd_prefix = "ART";
    char prefix[MAX_ATCMD_PREFIX] = {0};
    char single;
    int index = -1;

    // Eat "ART"
    at.parse(prefix, strlen(cmd_prefix));
    if (memcmp(prefix, cmd_prefix, strlen(cmd_prefix)) != 0) {
        LOGE(TAG, "invalid uart prefix %s\n", prefix);
        return;
    }

    at.parse(&single, 1);

    switch (single) {
        case 'E':
            index = ATCMD_UART_ECHO;
            break;

        default:
            LOGE(TAG, "invalid uart prefix %c\n", single);
            break;
    }

    if (index >= 0) {
        return &at_cmds_table[index];
    }

    return NULL;
}

static struct at_cli_command *get_atcmd_wifi_handler()
{
    char prefix[MAX_ATCMD_PREFIX] = {0};
    char single;
    int index = -1;

    at.parse(&single, 1);

    switch (single) {
        case 'J':
            // Eat AP
            at.parse(prefix, 2);
            if (memcmp(prefix, "AP", 2) != 0) {
                LOGE(TAG, "invalid wifi prefix %s\n", prefix);
                break;
            }

            at.parse(&single, 1);
            if (single ==  '=') {
                index = ATCMD_WJAP_CONN;
            } else if (single == 'I') {
                at.parse(prefix, 2);
                if (memcmp(prefix, "P?", 2) == 0) {
                    index = ATCMD_WJAP_IP;
                } else {
                    LOGE(TAG, "invalid wifi prefix %s\n", prefix);
                }
            } else {
                LOGE(TAG, "invalid wifi prefix %c\n", single);
            }

            break;

        case 'M':
            at.parse(prefix, 3);
            if (memcmp(prefix, "AC?", 3) == 0) {
                index = ATCMD_WJAP_MAC;
            } else {
                LOGE(TAG, "invalid wifi prefix %s\n", prefix);
            }
            break;

        default: {
            LOGE(TAG, "invalid wifi prefix %c\n", single);
            break;
        }
    }

    if (index >= 0) {
        return &at_cmds_table[index];
    }

    return NULL;
}

static struct at_cli_command *get_atcmd_ywss_handler()
{
    char prefix[MAX_ATCMD_PREFIX] = {0};
    char *single;
    int index = 0, len = 0, cmdidx = -1;;

    LOGD(TAG, "Hello %s entry", __func__);

    len = 1;
    prefix[index] = 'Y';
    index += len;

    len = strlen("WSS");
    at.parse(prefix + index, len);
    if (strcmp(prefix + index, "WSS") != 0) {
        LOGE(TAG, "invalid cmd prefix found (%s)", prefix);
        return NULL;
    }
    index += len;

    len = 1;
    single = prefix + index;
    at.parse(single, len);
    switch (*single) {
        case 'S':
            index += len;
            len = 3;
            at.parse(prefix + index, len);
            if (strcmp(prefix + index, "TOP") == 0) { /* AT+YWSSSTOPMONITOR */
                index += len;

                len = strlen("MONITOR"AT_SEND_DELIMITER);
                at.parse(prefix + index, len);
                if (strcmp(prefix + index, "MONITOR"AT_SEND_DELIMITER) != 0) {
                    LOGE(TAG, "invalid cmd prefix found (%s)", prefix);
                    break;
                }

                index += len;
                cmdidx = ATCMD_YWSS_STOP_MONITOR;
            } else if (strcmp(prefix + index, "TAR") == 0) {/* AT+YWSSSTARTMONITOR */
                index += len;

                len = strlen("TMONITOR"AT_SEND_DELIMITER);
                at.parse(prefix + index, len);
                if (strcmp(prefix + index, "TMONITOR"AT_SEND_DELIMITER) != 0) {
                    LOGE(TAG, "invalid cmd prefix found (%s)", prefix);
                    break;
                }

                index += len;
                cmdidx = ATCMD_YWSS_START_MONITOR;
            } else if (strcmp(prefix + index, "ETC") == 0) { /* AT+YWSSSETCHANNEL,<ch> */
                index += len;

                len = strlen("HANNEL,");
                at.parse(prefix + index, len);
                if (strcmp(prefix + index, "HANNEL,") != 0) {
                    LOGE(TAG, "invalid cmd prefix found (%s)", prefix);
                    break;
                }

                index += len;
                cmdidx = ATCMD_YWSS_SET_CHANNEL;
            } else if (strcmp(prefix + index, "USP") == 0) { // AT+YWSSSUSPENDSTATION
                index += len;

                len = strlen("ENDSTATION"AT_SEND_DELIMITER);
                at.parse(prefix + index, len);
                if (strcmp(prefix + index, "ENDSTATION"AT_SEND_DELIMITER) != 0) {
                    LOGE(TAG, "invalid cmd prefix found (%s)", prefix);
                    break;
                }

                index += len;
                cmdidx = ATCMD_YWSS_SUSPEND_STA;
            } else {
                LOGE(TAG, "invalid cmd prefix found (%s)", prefix);
            }
            break;
        default:
            LOGE(TAG, "invalid cmd prefix found (%s)", prefix);
            break;
    }

    LOGD(TAG, "cmd index is %d", cmdidx);

    LOGD(TAG, "Hello %s exit", __func__);

    return cmdidx < 0 ? NULL : &at_cmds_table[cmdidx];
}

static void atcmd_handler()
{
    char single;
    struct at_cli_command *handler = NULL;
    LOGD(TAG, "%s entry.", __func__);

    if (uart_echo() != 0) {
        return;
    }

    at.parse(&single, 1);

    switch (single) {
        case 'C':
            handler = get_atcmd_cip_handler();
            break;

        case 'U':
            handler = get_atcmd_uart_handler();
            break;

        case 'W':
            handler = get_atcmd_wifi_handler();
            break;
        //Add other cmd handles here

        case 'Y':
            handler = get_atcmd_ywss_handler();
            break;

        default:
            LOGE(TAG, "Unknown at command AT+%c\n", single);
            return;
    }

    if (handler != NULL) {
        handler->function();
    }

    LOGD(TAG, "%s exit.", __func__);
}

static void app_delayed_action(void *arg)
{
    LOG("AT host server: alive %s:%d %s\r\n", __func__, __LINE__, aos_task_name());
    aos_post_delayed_action(50000, app_delayed_action, NULL);
}

int application_start(int argc, char *argv[])
{
    at.set_mode(ASYN);
    // mk3060: 4096 mk3165: 1024
    at.set_worker_stack_size(4096);
    at.init(AT_RECV_PREFIX, AT_RECV_SUCCESS_POSTFIX, AT_RECV_FAIL_POSTFIX,
            AT_SEND_DELIMITER, 1000);

    athost_init();

    at.oob(prefix_athost,  NULL, 0, atcmd_handler, NULL);

    LOG("AT host server start!\n");
    aos_post_delayed_action(1000, app_delayed_action, NULL);

    aos_loop_run();

    return 0;
}
