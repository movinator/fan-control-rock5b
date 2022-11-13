/*
MIT License

Copyright (c) 2022 Nick Peng

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#define TMP_BUFF_LEN_32  32
#define DEFAULT_PID_PATH "/run/fan-control.pid"
#define DEFAULT_CFG_PATH "/etc/fan-control.cfg"
#define TEMP_PATH        "/sys/class/hwmon/hwmon%d/temp1_input"
#define PWM_BASE         "/sys/devices/platform/fd8b0010.pwm/pwm/pwmchip1/"
#define PWM_SUB(s)       PWM_BASE#s
#define PWM_FREQ         10000
int pidfile_fd = 0;
int temp_map[12][3];
int temp_max_size = 0;


int read_config(const char* config, char* buf, int bufsize) {
  int fd = open(config, O_RDONLY);
  int bytes_read = 0;

  if (fd < 0) {
     fprintf(stderr, "failed to open config file %s!\n", config);
     return -1;
     }
  if ((bytes_read = read(fd, buf, bufsize)) <= 0) {
     perror("read");
     return -2;
     }
  if (bytes_read < bufsize) buf[bytes_read] = '\0';
  else buf[bufsize - 1] = '\0';
  close(fd);

  return 0;
  }


int parse_config(char* buf, int bufsize) {
  const char *ep = buf + bufsize -1;
  char *p = buf, *sp = NULL;
  int   c=0, r=0, v=0;

  while (*p && p < ep) {
        switch (*p) {
          case '#':  // comment until end of line
               while (*p != '\n') ++p;
               if (p < ep && *p == '\n') ++p;
               continue;
          case '0':
          case '1':
          case '2':
          case '3':
          case '4':
          case '5':
          case '6':
          case '7':
          case '8':
          case '9':
               if (!sp) sp = p;
               ++p;
               continue;
          default:
               if (sp) break;
               ++p;
               continue;
          }
        *p++ = '\0';
        v = atoi(sp);
        temp_map[r][c] = v;
        if (++c > 2) {
           c = 0;
           if (++r > 11) break;
           }
        sp = NULL;
        }
  return r;
  }


int write_value(const char *file, const char *value) {
  int fd;

  fd = open(file, O_WRONLY);
  if (fd == -1) return -1;
  if (write(fd, value, strnlen(value, 16)) == -1) {
     close(fd);
     return -1;
     }
  close(fd);

  return 0;
  }


void write_speed(int speed) {
  char buffer[16];
  int freq = PWM_FREQ / 100 * temp_map[speed][1];

  printf("write speed (%d): %d\n"
       , speed
       , freq);
  snprintf(buffer, 15, "%d", freq);
  write_value(PWM_SUB(pwm0/duty_cycle), buffer);
  }


void set_speed(int speed) {
  static int last_speed = -1;

  if (speed < -1 || speed >= temp_max_size)
     speed = temp_max_size - 1;
  if (last_speed == speed) return;
  if (last_speed <= 0 && speed > 0) {
     write_speed(temp_max_size - 1);
     usleep(100000);
     }
  write_speed(speed);
  last_speed = speed;
  }


int get_speed(int temperature) {
  int i = 0;
  int speed = 0;
  static int last_speed = -1;
  static int last_temperature = -1;
  static int count = 0;

  for (i = temp_max_size - 1; i >= 0; --i) {
      if (temperature > temp_map[i][0]) {
         speed = i;
         if (last_speed < speed) {
            count = temp_map[i][2];
            }
         break;
         }
      }
  printf("get_speed() - temp: %d - count: %d - speed: %d\n"
       , temperature
       , count
       , speed);

  if (speed < last_speed) count--;
  else if (temperature > last_temperature) count++;

  if (count <= 0 || last_speed == -1 || last_speed < speed)
     last_speed = speed;
  last_temperature = temperature;

  return last_speed;
  }


int read_temp() {
  char buf[strlen(TEMP_PATH) + 5];
  char tempBuf[32];
  int  fdTemp = -1;
  int  temp = 0;
  int  value = 0;

  for (int i=1; i < 8; ++i) {
      snprintf(buf, sizeof(buf), TEMP_PATH, i);
//      printf("read temperature from %s\n", buf);
      if ((fdTemp = open(buf, O_RDONLY))) {
         lseek(fdTemp, 0, SEEK_SET);
         if (read(fdTemp, &tempBuf, 32) <= 0) {
            perror("read");
            continue;
            }
         value = atoi(tempBuf);
         close(fdTemp);
         }
     if (value > temp) temp = value;
     }
  return temp;
  }


void show_help(void) {
  char *msg = "PI custom fan control service.\n"
              "Usage: fan-control [option]\n"
              "Options:\n"
              "  -d       start as a daemon service.\n"
              "  -p       specify a pid file path (default: /run/fan-control.pid)\n"
              "  -c       specify a fan config file (default: /etc/fan-control.cfg)\n"
              "  -s [0-9] set fan speed.\n"
              "  -h       show help message.\n"
              "\n";
  printf("%s", msg);
  }


int init_GPIO() {
  int ret = 0;
  char max_speed[16];

  snprintf(max_speed, 15, "%d", PWM_FREQ);
  ret = write_value(PWM_SUB(export), "0");
  if (ret < 0 && errno != EBUSY) {
     printf("Failed to export PWM-module (%s), %s\n", PWM_SUB(export), strerror(errno));
     return -1;
     }
  ret = write_value(PWM_SUB(pwm0/duty_cycle), max_speed);
  if (ret < 0 && errno != EINVAL) {
     printf("Failed to set PWM duty cycle (%s), %s\n", PWM_SUB(pwm0/duty_cycle), strerror(errno));
     return -1;
     }
  ret = write_value(PWM_SUB(pwm0/period), max_speed);
  if (ret < 0) {
     printf("Failed to set PWM period (%s), %s\n", PWM_SUB(pwm0/period), strerror(errno));
     return -1;
     }
  ret = write_value(PWM_SUB(pwm0/polarity), "normal");
  if (ret < 0) {
     printf("Failed to set PWM polarity (%s), %s\n", PWM_SUB(pwm0/polarity), strerror(errno));
     return -1;
     }
  ret = write_value(PWM_SUB(pwm0/enable), "1");
  if (ret < 0) {
     printf("Failed to enable PWM (%s), %s\n", PWM_SUB(pwm0/enable), strerror(errno));
     return -1;
     }
  return 0;
  }


int create_pid_file(const char *pid_file) {
  int fd = 0;
  int flags = 0;
  char buff[TMP_BUFF_LEN_32];

  /*  create pid file, and lock this file */
  fd = open(pid_file, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  if (fd == -1) {
     fprintf(stderr, "create pid file failed, %s\n", strerror(errno));
     return -1;
    }
  flags = fcntl(fd, F_GETFD);
  if (flags < 0) {
     fprintf(stderr, "Could not get flags for PID file %s\n", pid_file);
     goto errout;
     }
  flags |= FD_CLOEXEC;
  if (fcntl(fd, F_SETFD, flags) == -1) {
     fprintf(stderr, "Could not set flags for PID file %s\n", pid_file);
     goto errout;
     }
  if (lockf(fd, F_TLOCK, 0) < 0) {
     fprintf(stderr, "Server is already running.\n");
     goto errout;
     }
  snprintf(buff, TMP_BUFF_LEN_32, "%d\n", getpid());

  if (write(fd, buff, strnlen(buff, TMP_BUFF_LEN_32)) < 0) {
     fprintf(stderr, "write pid to file failed, %s.\n", strerror(errno));
     goto errout;
     }
  if (pidfile_fd > 0) {
     close(pidfile_fd);
     }
  pidfile_fd = fd;

  return 0;

errout:
  if (fd > 0) close(fd);
  return -1;
  }


int main(int argc, char *argv[]) {
  char pid_file[1024] = {0};
  char cfg_file[1024] = {0};
  char buf[1024] = {0};
  int  cur_temp  = 0;
  int  cur_speed = -1;
  int  is_daemon = 0;
  int  rv = -1;
  int  opt;

  while ((opt = getopt(argc, argv, "s:p:dh")) != -1) {
        switch (opt) {
          case 's':
               cur_speed = atoi(optarg);
               break;
          case 'p':
               strncpy(pid_file, optarg, sizeof(pid_file) - 1);
               break;
          case 'c':
               strncpy(cfg_file, optarg, sizeof(cfg_file) - 1);
               break;
          case 'd':
               is_daemon = 1;
               break;
          case 'h':
               show_help();
               return 1;
          default:
               show_help();
               return 1;
          }
        }
  if (cfg_file[0] == '\0')
     strncpy(cfg_file, DEFAULT_CFG_PATH, sizeof(cfg_file) - 1);
  if (is_daemon) {
     if (daemon(0, 0) != 0) {
        printf("run daemon failed.\n");
        return 1;
        }
     if (pid_file[0] == '\0')
        strncpy(pid_file, DEFAULT_PID_PATH, sizeof(pid_file) - 1);
     if (create_pid_file(pid_file)) {
        return 1;
        }
     }

  if ((rv = read_config(cfg_file, buf, sizeof(buf)))) return rv;
  if (!(temp_max_size = parse_config(buf, sizeof(buf)))) return -2;
  printf("read %d temperature settings\n", temp_max_size);
  if (init_GPIO()) return 0;
  if (cur_speed != -1) {
     printf("Set speed to %d.\n", cur_speed);
     set_speed(cur_speed);

     return 0;
     }

  while (1) {
        sleep(1);
        cur_temp  = read_temp();
        cur_speed = get_speed(cur_temp / 1000);
        set_speed(cur_speed);

        if (!is_daemon) printf("speed:%d  temp:%d\n", cur_speed, cur_temp);
        }
  return 0;
  }
