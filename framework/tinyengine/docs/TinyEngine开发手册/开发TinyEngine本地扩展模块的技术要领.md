### 前提及背景：
前面介绍了如何使用C/C++快速开发一个TinyEngine的本地扩展模块，那么在开发过程中要注意哪些东西以及有哪些技术要领？本文将详细阐述。

### 名词解释
* #### 回调函数(callback)与事件处理(event)
回调和事件处理程序本质上并无区别，只是在不同情况下，不同的叫法
回调函数：程序（JS）费时的操作或函数需要回调通知 (异步通知)
事件处理：程序（JS）中的任务处理一般需要使用事件通知到JS任务
* __Javascript Addon 也叫Native module__
Javascript 的本地扩展模块，函数或对象，一般是用C语言实现的模块，在JS 层可以调用该模块，函数，或对象；

### 开发TinyEngine native 对象的技术要领
__说明__：<span data-type="color" style="color:#262626">具体实现可以参考 module_wdg.c、module_timers.c 等实现。</span>
* #### __<span data-type="color" style="color:#262626">如何产生异步调用（异步编程模型）？</span>__
耗时操作应该在一个new task中运行，通过aos\_schedule\_call把结果处理函数nextTick\_cb（C调用javascript函数）放到JavaScript主任务上下文去执行。
```c
    if(arg0 && symbol_is_function(arg0)){
        BE_ASYNC_S* async = (BE_ASYNC_S*)aos_malloc(sizeof(BE_ASYNC_S));
        async->func = arg0;
        async->param_count = 2;
        async->params = (be_jse_symbol_t**) aos_malloc( sizeof(be_jse_symbol_t*) * async->param_count);
        async->params[0] = new_int_symbol(count++);
        async->params[1] = new_int_symbol(count++);

        LOGW("WDG"," async = %p ", async);
        LOGW("WDG"," async->func = %p ", async->func);
        LOGW("WDG"," async->param_count = %d", async->param_count);
        LOGW("WDG"," async->params = %p ", async->params);


        ret = aos_schedule_call(nextTick_cb, async);
        if( ret >= 0 ) {
            // success
            INC_SYMBL_REF(async->func);
        }else {
            // 出错
            LOGW("WDG", "fatal error");
        }

    }

```
#### 
* #### __<span data-type="color" style="color:#262626">C 如何调用 JavaScript 的函数</span>__
       一般来说，本地扩展对象和函数是让上层 JS 应用调用 C 的函数，但是有时需要JSE 能够提供 C 反向调用JS函数，如事件通知，回调函数；
C 调用 Javascript 函数，一般采用 <strong><span data-type="color" style="color:#2F54EB">be_jse_execute_func</span></strong><strong> </strong>函数，__<span data-type="color" style="color:#2F54EB">be_jse_execute_func </span>__<span data-type="color" style="color:#262626">函数形式如下：</span>
```c
bool be_jse_execute_func(be_jse_executor_ctx_t *executor, be_jse_symbol_t *func, int argCount, be_jse_symbol_t **argPtr);
```
其中：
在C层调用JS函数时：
func 等于NULL时， argPtr[]会自动 symbol\_unlock
func执行结束之后，argPtr[]会自动 symbol\_unlock
若JS层函数参数数目与argCount不匹配时，argPtr[]会自动 symbol\_unlock

示例如下：
```c
static void nextTick_cb(void *arg)
{
    BE_ASYNC_S* async = (BE_ASYNC_S*)arg;
    int i;

    be_jse_execute_func(bone_engine_get_executor(), async->func, async->param_count, async->params);

    DEC_SYMBL_REF(async->func);

    if(async->params)
        aos_free(async->params);
    aos_free(async);
}
```

__注意事件:__
1.事件通知:
如何需要多次事件通知， func不能释放，可以参考module\_timers.c
2.`BE_ASYNC_S* async` 使用完之后需要释放

### <span data-type="color" style="color:#262626">如何创建构造数组？</span>
示例：
```c
int i;
be_jse_symbol_t *arr = new_symbol(BE_SYM_ARRAY);
for (i=0; i<5; i++) {
    be_jse_symbol_t *val = new_str_symbol("abcd");
    be_jse_symbol_t *idx = new_named_symbol(new_int_symbol(i), val);
    symbol_unlock(val);
    add_symbol_node(arr, idx);
    symbol_unlock(idx);
}
```

### <span data-type="color" style="color:#262626">如何创建构造对象？</span>
示例：
```c
be_jse_symbol_t *obj = new_symbol(BE_SYM_OBJECT);
be_jse_symbol_t *name = new_str_symbol("IoT");
be_jse_symbol_t *val = add_symbol_node_name(obj, name, "name");
symbol_unlock(name);
symbol_unlock(val);
```

* __根据JSON字符串构造对象或数组__
`be_jse_symbol_t *new_json_symbol(char* json_str, size_t json_str_len)`

### <span data-type="color" style="color:#262626">如何输出日志？</span>
建议使用AOS系统中的LOG输出系统
```c
定义在.c文件的第一个.h文件之前
#define CONFIG_LOGMACRO_DETAILS

使用
LOGD LOGW LOGE
```

#### __程序crash的调试方法__
例如程序出现crash的panic信息如下：
```bash
kernel panic,err 1200!
assertion "0" failed: file "platform/mcu/esp32/aos/soc_impl.c", line 35, function: soc_err_proc
abort() was called at PC 0x4013826b on core 0

Backtrace: 0x40088af0:0x3fffe4a0 0x40088bef:0x3fffe4c0 0x4013826b:0x3fffe4e0 0x4008b2c1:0x3fffe510 0x400859bd:0x3fffe530 0x40084706:0x3fffe550 0x4008b985:0x3fffe570 0x4008b962:0x3fffe590 0x400862cc:0x3fffe5b0 0x4008675f:0x3fffe5d0 0x40086bc8:0x3fffe5f0 0x400d3bee:0x3fffe610 0x400d3e82:0x3fffe640 0x400d26fb:0x3fffe660 0x40089f4e:0x3fffe6b0 0x401152a9:0x3fffe6e0 0x4011deec:0x3fffe700
```
解决方法：找到对应固件的elf文件，使用gdb调试
```plain

1. gdb加载对应的elf文件
./xtensa-esp32-elf-gdb.exe   ./gravity_lite@esp32devkitc.elf

2. 查看Backtrace中地址对应的函数
(gdb) info symbol 0x4008675f
krhino_sem_take + 135 in section .iram0.text
(gdb) info symbol 0x40086bc8
espos_sem_take + 12 in section .iram0.text
```


## 


 