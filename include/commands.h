#ifndef COMMANDS_H
#define COMMANDS_H

#include "protocol.h"   // Command 类型
#include "storage.h"    // Storage 类型
#include "zset.h"       // ZSet 类型
#include "server.h"     // send_response 声明（因为 commands.c 要用）

// 返回值：1 = 正常响应完成；-1 = 应断开连接（如 OOM：错误已发出，继续响应不安全，B8 修复）
int handle_command(int client_fd, Command *cmd, Storage *store, ZSet *zset);

#endif