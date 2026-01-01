#ifndef _TTS_LIST_H_
#define _TTS_LIST_H_

/*
语音功能模块	                          动作	                                播报内容              
通电使用	                             通电	                             欢迎使用好瞳伴            
照明灯氛围灯启用	                     开启上灯板	                          灯光亮起，照亮天际
                                  一键开启下灯板+氛围灯（设备端才有的操作）	     柔光洒落，温暖四方
                                    只开启氛围灯	                      氛围轻启，星光漫溢
                            一键上、下灯板和氛围灯全开（小程序才有的操作）	   全屋灯启，满室温馨
                                只开启下灯板（小程序才有的操作）	         柔光洒落，温暖四方
                                    书本阅读模式	                         书本阅读模式
                                    电子阅读模式	                         电子阅读模式
                                    开启红光模式	                           护眼模式
                                    开启专注模式	                           专注模式
灯光使用时长播报	            照明灯使用灯光时长超过20min	                   光影消散，休息一下吧
OTA升级功能	                           开始升级	                         OTA升级中，请勿关机
                                    升级成功	                             OTA升级成功
                            升级失败（升级过程中关机或断网无法播报）	          OTA升级失败
音乐播放	                    光疗模式、休息提醒开启音乐	                      追忆钢琴曲
*/

// 音频文件路径定义
#define WELCOME                 "/spiffs/001TTS.mp3"            // 欢迎使用好瞳伴   
#define UP_LIGHT                "/spiffs/002TTS.mp3"            // 灯光亮起，照亮天际
#define DOWM_LIGHT              "/spiffs/003TTS.mp3"            // 柔光洒落，温暖四方
#define AROUND_LIGHT            "/spiffs/004TTS.mp3"            // 氛围轻启，星光漫溢
#define ALL_LIGHT               "/spiffs/005TTS.mp3"            // 全屋灯启，满室温馨
#define BOOK_READ               "/spiffs/006TTS.mp3"            // 书本阅读模式
#define ELEC_READ               "/spiffs/007TTS.mp3"            // 电子阅读模式
#define CARE_MODE               "/spiffs/008TTS.mp3"            // 护眼模式
#define FOCUS_MODE              "/spiffs/009TTS.mp3"            // 专注模式
#define TURN_OFF                "/spiffs/010TTS.mp3"            // 光影消散，休息一下吧
#define OTA_UPDATE              "/spiffs/011TTS.mp3"            // OTA升级中，请勿关机
#define OTA_SUCCESS             "/spiffs/012TTS.mp3"            // OTA升级成功
#define OTA_FAIL                "/spiffs/013TTS.mp3"            // OTA升级失败
#define A2DP_OPEN               "/spiffs/014TTS.mp3"            // 蓝牙音箱已开启
#define A2DP_CLOSE              "/spiffs/015TTS.mp3"            // 蓝牙音箱已关闭
#define SLEEPING_MODE           "/spiffs/016TTS.mp3"            // 睡眠模式

#define MUSIC                   "/spiffs/music.mp3"
#define MUSIC_40HZ              "/spiffs/music_40hz.mp3"

#endif
