/*
 * crun - OCI runtime written in C
 *
 * Copyright (C) 2017, 2018, 2019 Giuseppe Scrivano <giuseppe@scrivano.org>
 * crun is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * crun is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with crun.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE

#include <config.h>
#include "cgroup.h"
#include "cgroup-internal.h"
#include "cgroup-systemd.h"
#include "cgroup-utils.h"
#include "cgroup-resources.h"
#include "ebpf.h"
#include "utils.h"
#include "status.h"
#include <string.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <inttypes.h>
#include <time.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <libgen.h>

struct default_dev_s default_devices[] = {
  { 'c', -1, -1, "m" },
  { 'b', -1, -1, "m" },
  { 'c', 1, 3, "rwm" },
  { 'c', 1, 8, "rwm" },
  { 'c', 1, 7, "rwm" },
  { 'c', 5, 0, "rwm" },
  { 'c', 1, 5, "rwm" },
  { 'c', 1, 9, "rwm" },
  { 'c', 5, 1, "rwm" },
  { 'c', 136, -1, "rwm" },
  { 'c', 5, 2, "rwm" },
  { 'c', 10, 200, "rwm" },
  { 0, 0, 0, NULL }
};

struct default_dev_s *
get_default_devices ()
{
  return default_devices;
}

static inline int
write_cgroup_file (int dirfd, const char *name, const void *data, size_t len, libcrun_error_t *err)
{
  return write_file_at_with_flags (dirfd, O_WRONLY | O_CLOEXEC, 0, name, data, len, err);
}

static int
write_cgroup_file_or_alias (int dirfd, const char *name, const char *alias, const void *data, size_t len, libcrun_error_t *err)
{
  int ret;

  ret = write_cgroup_file (dirfd, name, data, len, err);
  if (UNLIKELY (alias != NULL && ret < 0 && crun_error_get_errno (err) == ENOENT))
    {
      crun_error_release (err);
      ret = write_cgroup_file (dirfd, alias, data, len, err);
    }
  return ret;
}

static inline int
openat_with_alias (int dirfd, const char *name, const char *alias, const char **used_name, int flags, libcrun_error_t *err)
{
  int ret;

  *used_name = name;

  ret = openat (dirfd, name, flags);
  if (UNLIKELY (ret < 0 && alias != NULL && errno == ENOENT))
    {
      *used_name = alias;
      ret = openat (dirfd, alias, flags);
    }
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "open `%s`", name);
  return ret;
}

static int
is_rwm (const char *str, libcrun_error_t *err)
{
  const char *it;
  bool r = false;
  bool w = false;
  bool m = false;

  for (it = str; *it; it++)
    switch (*it)
      {
      case 'r':
        r = true;
        break;

      case 'w':
        w = true;
        break;

      case 'm':
        m = true;
        break;

      default:
        return crun_make_error (err, 0, "invalid mode specified `%s`", str);
      }

  return r && w && m ? 1 : 0;
}

static int
check_cgroup_v2_controller_available_wrapper (int ret, int cgroup_dirfd, const char *name, libcrun_error_t *err)
{
  if (ret == 0 || err == NULL)
    return 0;

  errno = crun_error_get_errno (err);

  /* If the file is not found, try to give a more meaningful error message.  */
  if (errno == ENOENT || errno == EPERM || errno == EACCES)
    {
      cleanup_free char *controllers = NULL;
      libcrun_error_t tmp_err = NULL;
      cleanup_free char *key = NULL;
      char *saveptr = NULL;
      bool found = false;
      const char *token;
      char *it;

      /* Check if the specified controller is enabled.  */
      key = xstrdup (name);

      it = strchr (key, '.');
      if (it == NULL)
        {
          crun_error_release (err);
          return crun_make_error (err, 0, "the specified key has not the form CONTROLLER.VALUE `%s`", name);
        }
      *it = '\0';

      /* cgroup. files are not part of a controller.  Return the original error.  */
      if (strcmp (key, "cgroup") == 0)
        return ret;

      /* If the cgroup.controllers file cannot be read, return the original error.  */
      if (read_all_file_at (cgroup_dirfd, "cgroup.controllers", &controllers, NULL, &tmp_err) < 0)
        {
          crun_error_release (&tmp_err);
          return ret;
        }
      for (token = strtok_r (controllers, " \n", &saveptr); token; token = strtok_r (NULL, " \n", &saveptr))
        {
          if (strcmp (token, key) == 0)
            {
              found = true;
              break;
            }
        }
      if (! found)
        {
          cleanup_free char *absolute_path = NULL;
          libcrun_error_t tmp_err = NULL;

          crun_error_release (err);
          ret = get_realpath_to_file (cgroup_dirfd, "cgroup.controllers", &absolute_path, &tmp_err);
          if (LIKELY (ret >= 0))
            ret = crun_make_error (err, 0, "controller `%s` is not available under %s", key, absolute_path);
          else
            {
              crun_error_release (&tmp_err);
              ret = crun_make_error (err, 0, "the requested cgroup controller `%s` is not available", key);
            }
        }
    }
  return ret;
}

static int
write_file_and_check_controllers_at (bool cgroup2, int dirfd, const char *name, const char *name_alias,
                                     const void *data, size_t len, libcrun_error_t *err)
{
  int ret;

  ret = write_cgroup_file_or_alias (dirfd, name, name_alias, data, len, err);
  if (cgroup2)
    return check_cgroup_v2_controller_available_wrapper (ret, dirfd, name, err);
  return ret;
}

static int
open_file_and_check_controllers_at (bool cgroup2, int dirfd, const char *name, int flags, libcrun_error_t *err)
{
  int ret;

  ret = openat (dirfd, name, flags);
  if (UNLIKELY (ret < 0))
    {
      ret = crun_make_error (err, errno, "open `%s`", name);
      if (cgroup2)
        return check_cgroup_v2_controller_available_wrapper (ret, dirfd, name, err);
    }
  return ret;
}

/* The parser generates different structs but they are really all the same.  */
typedef runtime_spec_schema_defs_linux_block_io_device_throttle throttling_s;

static int
write_blkio_v1_resources_throttling (int dirfd, const char *name, throttling_s **throttling, size_t throttling_len,
                                     libcrun_error_t *err)
{
  char fmt_buf[128];
  size_t i;
  cleanup_close int fd = -1;

  if (throttling == NULL)
    return 0;

  fd = openat (dirfd, name, O_WRONLY | O_CLOEXEC);
  if (UNLIKELY (fd < 0))
    return crun_make_error (err, errno, "open `%s`", name);

  for (i = 0; i < throttling_len; i++)
    {
      int ret;
      int len;
      len = snprintf (fmt_buf, sizeof (fmt_buf), "%" PRIu64 ":%" PRIu64 " %" PRIu64 "\n", throttling[i]->major, throttling[i]->minor,
                      throttling[i]->rate);
      if (UNLIKELY (len >= (int) sizeof (fmt_buf)))
        return crun_make_error (err, 0, "internal error: static buffer too small");

      ret = TEMP_FAILURE_RETRY (write (fd, fmt_buf, len));
      if (UNLIKELY (ret < 0))
        return crun_make_error (err, errno, "write `%s`", name);
    }
  return 0;
}

static int
write_blkio_v2_resources_throttling (int fd, const char *name, throttling_s **throttling, size_t throttling_len,
                                     libcrun_error_t *err)
{
  char fmt_buf[128];
  size_t i;

  if (throttling == NULL)
    return 0;

  for (i = 0; i < throttling_len; i++)
    {
      int ret;
      int len;
      len = snprintf (fmt_buf, sizeof (fmt_buf), "%" PRIu64 ":%" PRIu64 " %s=%" PRIu64 "\n", throttling[i]->major, throttling[i]->minor,
                      name, throttling[i]->rate);
      if (UNLIKELY (len >= (int) sizeof (fmt_buf)))
        return crun_make_error (err, 0, "internal error: static buffer too small");

      ret = TEMP_FAILURE_RETRY (write (fd, fmt_buf, len));
      if (UNLIKELY (ret < 0))
        return crun_make_error (err, errno, "write `%s`", name);
    }
  return 0;
}

static int
write_blkio_resources (int dirfd, bool cgroup2, runtime_spec_schema_config_linux_resources_block_io *blkio,
                       libcrun_error_t *err)
{
  char fmt_buf[128];
  int len;
  int ret;

  if (blkio->weight)
    {
      uint32_t val = blkio->weight;

      len = snprintf (fmt_buf, sizeof (fmt_buf), "%" PRIu32, val);
      if (UNLIKELY (len >= (int) sizeof (fmt_buf)))
        return crun_make_error (err, 0, "internal error: static buffer too small");
      if (! cgroup2)
        {
          ret = write_cgroup_file_or_alias (dirfd, "blkio.weight", "blkio.bfq.weight", fmt_buf, len, err);
          if (UNLIKELY (ret < 0))
            return ret;
        }
      else
        {
          ret = write_cgroup_file (dirfd, "io.bfq.weight", fmt_buf, len, err);
          if (UNLIKELY (ret < 0))
            {
              if (crun_error_get_errno (err) == ENOENT)
                {
                  crun_error_release (err);

                  /* convert linearly from [10-1000] to [1-10000] */
                  val = 1 + (val - 10) * 9999 / 990;

                  len = snprintf (fmt_buf, sizeof (fmt_buf), "%" PRIu32, val);
                  if (UNLIKELY (len >= (int) sizeof (fmt_buf)))
                    return crun_make_error (err, 0, "internal error: static buffer too small");

                  ret = write_cgroup_file (dirfd, "io.weight", fmt_buf, len, err);
                }

              if (UNLIKELY (ret < 0))
                return ret;
            }
        }
    }
  if (blkio->leaf_weight)
    {
      if (cgroup2)
        return crun_make_error (err, 0, "cannot set leaf_weight with cgroupv2");

      len = snprintf (fmt_buf, sizeof (fmt_buf), "%d", blkio->leaf_weight);
      if (UNLIKELY (len >= (int) sizeof (fmt_buf)))
        return crun_make_error (err, 0, "internal error: static buffer too small");

      ret = write_cgroup_file (dirfd, "blkio.leaf_weight", fmt_buf, len, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }
  if (blkio->weight_device_len)
    {
      if (cgroup2)
        {
          cleanup_close int wfd = -1;
          size_t i;

          wfd = openat (dirfd, "io.bfq.weight", O_WRONLY | O_CLOEXEC);
          if (UNLIKELY (wfd < 0))
            return crun_make_error (err, errno, "open `io.bfq.weight`");
          for (i = 0; i < blkio->weight_device_len; i++)
            {
              uint32_t w = blkio->weight_device[i]->weight;

              len = snprintf (fmt_buf, sizeof (fmt_buf), "%" PRIu64 ":%" PRIu64 " %i\n", blkio->weight_device[i]->major,
                              blkio->weight_device[i]->minor, w);
              if (UNLIKELY (len >= (int) sizeof (fmt_buf)))
                return crun_make_error (err, 0, "internal error: static buffer too small");

              ret = TEMP_FAILURE_RETRY (write (wfd, fmt_buf, len));
              if (UNLIKELY (ret < 0))
                return crun_make_error (err, errno, "write `io.bfq.weight`");

              /* Ignore blkio->weight_device[i]->leaf_weight.  */
            }
        }
      else
        {
          const char *leaf_weight_device_file_name = NULL;
          const char *weight_device_file_name = NULL;
          cleanup_close int w_leafdevice_fd = -1;
          cleanup_close int w_device_fd = -1;
          size_t i;

          w_device_fd = openat_with_alias (dirfd, "blkio.weight_device", "blkio.bfq.weight_device",
                                           &weight_device_file_name, O_WRONLY | O_CLOEXEC, err);
          if (UNLIKELY (w_device_fd < 0))
            return w_device_fd;

          w_leafdevice_fd = openat_with_alias (dirfd, "blkio.leaf_weight_device", "blkio.bfq.leaf_weight_device",
                                               &leaf_weight_device_file_name, O_WRONLY | O_CLOEXEC, err);
          if (UNLIKELY (w_leafdevice_fd < 0))
            {
              /* If the .leaf_weight_device file is missing, just ignore it.  */
              crun_error_release (err);
            }

          for (i = 0; i < blkio->weight_device_len; i++)
            {
              len = snprintf (fmt_buf, sizeof (fmt_buf), "%" PRIu64 ":%" PRIu64 " %" PRIu16 "\n", blkio->weight_device[i]->major,
                              blkio->weight_device[i]->minor, blkio->weight_device[i]->weight);
              if (UNLIKELY (len >= (int) sizeof (fmt_buf)))
                return crun_make_error (err, 0, "internal error: static buffer too small");

              ret = TEMP_FAILURE_RETRY (write (w_device_fd, fmt_buf, len));
              if (UNLIKELY (ret < 0))
                return crun_make_error (err, errno, "write `%s`", weight_device_file_name);

              if (w_leafdevice_fd >= 0)
                {
                  len = snprintf (fmt_buf, sizeof (fmt_buf), "%" PRIu64 ":%" PRIu64 " %" PRIu16 "\n", blkio->weight_device[i]->major,
                                  blkio->weight_device[i]->minor, blkio->weight_device[i]->leaf_weight);
                  if (UNLIKELY (len >= (int) sizeof (fmt_buf)))
                    return crun_make_error (err, 0, "internal error: static buffer too small");

                  ret = TEMP_FAILURE_RETRY (write (w_leafdevice_fd, fmt_buf, len));
                  if (UNLIKELY (ret < 0))
                    return crun_make_error (err, errno, "write `%s`", leaf_weight_device_file_name);
                }
            }
        }
    }
  if (cgroup2)
    {
      cleanup_close int wfd = -1;
      const char *name = "io.max";

      wfd = openat (dirfd, name, O_WRONLY | O_CLOEXEC);
      if (UNLIKELY (wfd < 0))
        {
          ret = crun_make_error (err, errno, "open `%s`", name);
          return check_cgroup_v2_controller_available_wrapper (ret, dirfd, name, err);
        }

      ret = write_blkio_v2_resources_throttling (wfd, "rbps", (throttling_s **) blkio->throttle_read_bps_device,
                                                 blkio->throttle_read_bps_device_len, err);
      if (UNLIKELY (ret < 0))
        return ret;

      ret = write_blkio_v2_resources_throttling (wfd, "wbps", (throttling_s **) blkio->throttle_write_bps_device,
                                                 blkio->throttle_write_bps_device_len, err);
      if (UNLIKELY (ret < 0))
        return ret;

      ret = write_blkio_v2_resources_throttling (wfd, "riops", (throttling_s **) blkio->throttle_read_iops_device,
                                                 blkio->throttle_read_iops_device_len, err);
      if (UNLIKELY (ret < 0))
        return ret;

      ret = write_blkio_v2_resources_throttling (wfd, "wiops", (throttling_s **) blkio->throttle_write_iops_device,
                                                 blkio->throttle_write_iops_device_len, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }
  else
    {
      ret = write_blkio_v1_resources_throttling (dirfd, "blkio.throttle.read_bps_device",
                                                 (throttling_s **) blkio->throttle_read_bps_device,
                                                 blkio->throttle_read_bps_device_len, err);
      if (UNLIKELY (ret < 0))
        return ret;

      ret = write_blkio_v1_resources_throttling (dirfd, "blkio.throttle.write_bps_device",
                                                 (throttling_s **) blkio->throttle_write_bps_device,
                                                 blkio->throttle_write_bps_device_len, err);
      if (UNLIKELY (ret < 0))
        return ret;

      ret = write_blkio_v1_resources_throttling (dirfd, "blkio.throttle.read_iops_device",
                                                 (throttling_s **) blkio->throttle_read_iops_device,
                                                 blkio->throttle_read_iops_device_len, err);
      if (UNLIKELY (ret < 0))
        return ret;

      ret = write_blkio_v1_resources_throttling (dirfd, "blkio.throttle.write_iops_device",
                                                 (throttling_s **) blkio->throttle_write_iops_device,
                                                 blkio->throttle_write_iops_device_len, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }
  return 0;
}

static int
write_network_resources (int dirfd_netclass, int dirfd_netprio, runtime_spec_schema_config_linux_resources_network *net,
                         libcrun_error_t *err)
{
  char fmt_buf[128];
  int len;
  int ret;
  if (net->class_id)
    {
      len = snprintf (fmt_buf, sizeof (fmt_buf), "%d", net->class_id);
      if (UNLIKELY (len >= (int) sizeof (fmt_buf)))
        return crun_make_error (err, 0, "internal error: static buffer too small");

      ret = write_cgroup_file (dirfd_netclass, "net_cls.classid", fmt_buf, len, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }
  if (net->priorities_len)
    {
      size_t i;
      cleanup_close int fd = -1;
      fd = openat (dirfd_netprio, "net_prio.ifpriomap", O_WRONLY | O_CLOEXEC);
      if (UNLIKELY (fd < 0))
        return crun_make_error (err, errno, "open `net_prio.ifpriomap`");

      for (i = 0; i < net->priorities_len; i++)
        {
          len = snprintf (fmt_buf, sizeof (fmt_buf), "%s %d\n", net->priorities[i]->name, net->priorities[i]->priority);
          if (UNLIKELY (len >= (int) sizeof (fmt_buf)))
            return crun_make_error (err, 0, "internal error: static buffer too small");

          ret = TEMP_FAILURE_RETRY (write (fd, fmt_buf, len));
          if (UNLIKELY (ret < 0))
            return crun_make_error (err, errno, "write `net_prio.ifpriomap`");
        }
    }

  return 0;
}

static int
write_hugetlb_resources (int dirfd, bool cgroup2,
                         runtime_spec_schema_config_linux_resources_hugepage_limits_element **htlb, size_t htlb_len,
                         libcrun_error_t *err)
{
  char fmt_buf[128];
  size_t i;
  for (i = 0; i < htlb_len; i++)
    {
      cleanup_free char *filename = NULL;
      const char *suffix;
      int len;
      int ret;

      suffix = cgroup2 ? "max" : "limit_in_bytes";

      xasprintf (&filename, "hugetlb.%s.%s", htlb[i]->page_size, suffix);

      len = snprintf (fmt_buf, sizeof (fmt_buf), "%" PRIu64, htlb[i]->limit);
      if (UNLIKELY (len >= (int) sizeof (fmt_buf)))
        return crun_make_error (err, 0, "internal error: static buffer too small");
      ret = write_file_and_check_controllers_at (cgroup2, dirfd, filename, NULL, fmt_buf, len, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }
  return 0;
}

static int
write_devices_resources_v1 (int dirfd, runtime_spec_schema_defs_linux_device_cgroup **devs, size_t devs_len,
                            libcrun_error_t *err)
{
  size_t i;
  int len;
  int ret;

  for (i = 0; i < devs_len; i++)
    {
      /* It is plenty of room for "TYPE MAJOR:MINOR ACCESS", where type is one char, and ACCESS is at most 3.   */
#define FMT_BUF_LEN 64
      char fmt_buf[FMT_BUF_LEN];
      const char *file = devs[i]->allow ? "devices.allow" : "devices.deny";

      if (devs[i]->type == NULL || devs[i]->type[0] == 'a')
        {
          strcpy (fmt_buf, "a");
          len = 1;
        }
      else
        {
          char fmt_buf_major[16];
          char fmt_buf_minor[16];

#define FMT_DEV(x, b)                                                                   \
  do                                                                                    \
    {                                                                                   \
      if (x##_present)                                                                  \
        {                                                                               \
          len = snprintf (b, sizeof (b), "%" PRIi64, x);                                \
          if (UNLIKELY (len >= (int) sizeof (b)))                                       \
            return crun_make_error (err, 0, "internal error: static buffer too small"); \
        }                                                                               \
      else                                                                              \
        strcpy (b, "*");                                                                \
  } while (0)

          FMT_DEV (devs[i]->major, fmt_buf_major);
          FMT_DEV (devs[i]->minor, fmt_buf_minor);
#undef FMT_DEV

          len = snprintf (fmt_buf, FMT_BUF_LEN, "%s %s:%s %s", devs[i]->type, fmt_buf_major, fmt_buf_minor,
                          devs[i]->access);
          if (UNLIKELY (len >= FMT_BUF_LEN))
            return crun_make_error (err, 0, "internal error: static buffer too small");
        }
      ret = write_cgroup_file (dirfd, file, fmt_buf, len, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  for (i = 0; default_devices[i].type; i++)
    {
      char fmt_buf_major[16];
      char fmt_buf_minor[16];
      char device[64];
      int len;

#define FMT_DEV(x, b)                                                                   \
  do                                                                                    \
    {                                                                                   \
      if (x != -1)                                                                      \
        {                                                                               \
          len = snprintf (b, sizeof (b), "%d", x);                                      \
          if (UNLIKELY (len >= (int) sizeof (b)))                                       \
            return crun_make_error (err, 0, "internal error: static buffer too small"); \
        }                                                                               \
      else                                                                              \
        strcpy (b, "*");                                                                \
  } while (0)

      FMT_DEV (default_devices[i].major, fmt_buf_major);
      FMT_DEV (default_devices[i].minor, fmt_buf_minor);

#undef FMT_DEV

      len = snprintf (device, sizeof (device), "%c %s:%s %s", default_devices[i].type, fmt_buf_major, fmt_buf_minor,
                      default_devices[i].access);
      if (UNLIKELY (len >= (int) sizeof (device)))
        return crun_make_error (err, 0, "internal error: static buffer too small");

      ret = write_cgroup_file (dirfd, "devices.allow", device, strlen (device), err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  return 0;
}

struct bpf_program *
create_dev_bpf (runtime_spec_schema_defs_linux_device_cgroup **devs, size_t devs_len,
                libcrun_error_t *err)
{
  int i;
  struct bpf_program *program;

  program = bpf_program_new (2048);

  program = bpf_program_init_dev (program, err);
  if (UNLIKELY (program == NULL))
    return NULL;

  for (i = (sizeof (default_devices) / sizeof (default_devices[0])) - 1; i >= 0; i--)
    {
      program = bpf_program_append_dev (program, default_devices[i].access, default_devices[i].type,
                                        default_devices[i].major, default_devices[i].minor, true, err);
      if (UNLIKELY (program == NULL))
        return NULL;
    }
  for (i = devs_len - 1; i >= 0; i--)
    {
      char type = 'a';
      int minor = -1, major = -1;
      if (devs[i]->type != NULL)
        type = devs[i]->type[0];

      if (devs[i]->major_present)
        major = devs[i]->major;
      if (devs[i]->minor_present)
        minor = devs[i]->minor;

      program = bpf_program_append_dev (program, devs[i]->access, type, major, minor, devs[i]->allow, err);
      if (UNLIKELY (program == NULL))
        return NULL;
    }

  program = bpf_program_complete_dev (program, err);
  if (UNLIKELY (program == NULL))
    return NULL;

  return program;
}

static int
write_devices_resources_v2_internal (int dirfd, runtime_spec_schema_defs_linux_device_cgroup **devs, size_t devs_len,
                                     libcrun_error_t *err)
{
  cleanup_free struct bpf_program *program = NULL;

  program = create_dev_bpf (devs, devs_len, err);
  if (UNLIKELY (program == NULL))
    return -1;

  return libcrun_ebpf_load (program, dirfd, NULL, err);
}

static int
write_devices_resources_v2 (int dirfd, runtime_spec_schema_defs_linux_device_cgroup **devs, size_t devs_len,
                            libcrun_error_t *err)
{
  int ret;
  size_t i;
  bool can_skip = true;

  ret = write_devices_resources_v2_internal (dirfd, devs, devs_len, err);
  if (LIKELY (ret == 0))
    return 0;

  /* If writing the resources ebpf failed, check if it is fine to ignore the error.  */
  for (i = 0; i < devs_len; i++)
    {
      if (devs[i]->allow_present && ! devs[i]->allow)
        {
          can_skip = false;
          break;
        }
    }

  if (! can_skip)
    {
      libcrun_error_t tmp_err = NULL;
      int rootless;

      rootless = is_rootless (&tmp_err);
      if (UNLIKELY (rootless < 0))
        {
          crun_error_release (err);
          *err = tmp_err;
          return ret;
        }
      if (rootless)
        can_skip = true;
    }

  if (can_skip)
    {
      crun_error_release (err);
      ret = 0;
    }

  return ret;
}

static int
write_devices_resources (int dirfd, bool cgroup2, runtime_spec_schema_defs_linux_device_cgroup **devs, size_t devs_len,
                         libcrun_error_t *err)
{
  int ret;

  if (cgroup2)
    ret = write_devices_resources_v2 (dirfd, devs, devs_len, err);
  else
    ret = write_devices_resources_v1 (dirfd, devs, devs_len, err);
  if (UNLIKELY (ret < 0))
    {
      libcrun_error_t tmp_err = NULL;
      int rootless;

      rootless = is_rootless (&tmp_err);
      if (UNLIKELY (rootless < 0))
        {
          crun_error_release (&tmp_err);
          return ret;
        }

      if (rootless)
        {
          crun_error_release (err);
          ret = 0;
        }
    }
  return ret;
}

/* use for cgroupv2 files with .min, .max, .low, or .high suffix */
static int
cg_itoa (char *buf, size_t size, int64_t value, bool use_max, libcrun_error_t *err)
{
  int len;

  if (use_max && value < 0)
    len = snprintf (buf, size, "max");
  else
    len = snprintf (buf, size, "%" PRIi64, value);

  if (UNLIKELY (len >= (int) size))
    return crun_make_error (err, 0, "internal error: static buffer too small");

  return len;
}

static int
write_memory (int dirfd, bool cgroup2, runtime_spec_schema_config_linux_resources_memory *memory, libcrun_error_t *err)
{
  char limit_buf[32];
  int limit_buf_len;

  if (! memory->limit_present)
    return 0;

  limit_buf_len = cg_itoa (limit_buf, sizeof (limit_buf), memory->limit, cgroup2, err);
  if (UNLIKELY (limit_buf_len < 0))
    return limit_buf_len;

  return write_cgroup_file (dirfd, cgroup2 ? "memory.max" : "memory.limit_in_bytes", limit_buf, limit_buf_len, err);
}

static int
write_memory_swap (int dirfd, bool cgroup2, runtime_spec_schema_config_linux_resources_memory *memory,
                   libcrun_error_t *err)
{
  int ret;
  int64_t swap;
  char swap_buf[32];
  int len;
  const char *fname = cgroup2 ? "memory.swap.max" : "memory.memsw.limit_in_bytes";

  if (! memory->swap_present)
    return 0;

  swap = memory->swap;
  // Cgroupv2 apply limit must check if swap > 0, since `0` and `-1` are special case
  // 0: This means process will not be able to use any swap space.
  // -1: This means that the process can use as much swap as it needs.
  if (cgroup2 && memory->swap > 0)
    {
      if (! memory->limit_present)
        return crun_make_error (err, 0, "cannot set swap limit without the memory limit");
      if (memory->swap < memory->limit)
        return crun_make_error (err, 0, "cannot set memory+swap limit less than the memory limit");

      swap -= memory->limit;
    }

  len = cg_itoa (swap_buf, sizeof (swap_buf), swap, cgroup2, err);
  if (UNLIKELY (len < 0))
    return len;

  ret = write_cgroup_file (dirfd, fname, swap_buf, len, err);
  if (ret >= 0)
    return ret;

  /* If swap is not enabled, ignore the error.  */
  if (crun_error_get_errno (err) == ENOENT)
    {
      crun_error_release (err);
      return 0;
    }

  return ret;
}

static int
write_memory_resources (int dirfd, bool cgroup2, runtime_spec_schema_config_linux_resources_memory *memory,
                        libcrun_error_t *err)
{
  int len;
  int ret;
  char fmt_buf[32];
  bool memory_limits_written = false;

  if (cgroup2 && memory->check_before_update_present && memory->check_before_update)
    {
      cleanup_free char *swap_current = NULL;
      cleanup_free char *current = NULL;
      uint64_t limit = 0;
      uint64_t val, val_swap;
      int ret;

      ret = read_all_file_at (dirfd, "memory.current", &current, NULL, err);
      if (UNLIKELY (ret < 0))
        return ret;

      ret = read_all_file_at (dirfd, "memory.swap.current", &swap_current, NULL, err);
      if (UNLIKELY (ret < 0))
        return ret;

      errno = 0;
      val = strtoll (current, NULL, 10);
      if (UNLIKELY (errno))
        return crun_make_error (err, errno, "parse memory.current");

      val_swap = strtoll (swap_current, NULL, 10);
      if (UNLIKELY (errno))
        return crun_make_error (err, errno, "parse memory.swap.current");

      if (memory->limit_present && memory->limit >= 0)
        limit = memory->limit;
      if (memory->swap_present && memory->swap >= 0)
        limit += memory->swap;

      if (limit <= val + val_swap)
        return crun_make_error (err, 0, "cannot set the memory limit lower than its current usage");
    }

  if (memory->limit_present)
    {
      ret = write_memory (dirfd, cgroup2, memory, err);
      if (ret >= 0)
        memory_limits_written = true;
      else
        {
          if (cgroup2 || crun_error_get_errno (err) != EINVAL)
            return ret;

          /*
            If we get an EINVAL error on cgroup v1 we reverse
            the order we write the memory limit and the swap.
            Attempt to write again the memory limit once the memory
            swap is written.
          */
          crun_error_release (err);
        }
    }

  ret = write_memory_swap (dirfd, cgroup2, memory, err);
  if (UNLIKELY (ret < 0))
    return ret;

  if (memory->limit_present && ! memory_limits_written)
    {
      ret = write_memory (dirfd, cgroup2, memory, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  if (memory->kernel_present)
    {
      if (cgroup2)
        return crun_make_error (err, 0, "cannot set kernel memory with cgroupv2");

      len = snprintf (fmt_buf, sizeof (fmt_buf), "%" PRIu64, memory->kernel);
      if (UNLIKELY (len >= (int) sizeof (fmt_buf)))
        return crun_make_error (err, 0, "internal error: static buffer too small");
      ret = write_cgroup_file (dirfd, "memory.kmem.limit_in_bytes", fmt_buf, len, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  // allows users set use_hierarchy as 0 when defined as false in spec.
  // if use_hierarchy is not defined in spec value defaults to 1 (True).
  // Note: users can only toggle use_hierarchy if the parent cgroup has use_hierarchy configured as 0.
  if (memory->use_hierarchy_present)
    {
      if (cgroup2)
        return crun_make_error (err, 0, "cannot set useHierarchy memory with cgroupv2");

      ret = write_cgroup_file (dirfd, "memory.use_hierarchy", (memory->use_hierarchy) ? "1" : "0", 1, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  if (memory->reservation_present)
    {
      len = snprintf (fmt_buf, sizeof (fmt_buf), "%" PRIu64, memory->reservation);
      if (UNLIKELY (len >= (int) sizeof (fmt_buf)))
        return crun_make_error (err, 0, "internal error: static buffer too small");
      ret = write_file_and_check_controllers_at (cgroup2, dirfd, cgroup2 ? "memory.low" : "memory.soft_limit_in_bytes",
                                                 NULL, fmt_buf, len, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }
  if (memory->disable_oom_killer)
    {
      if (cgroup2)
        return crun_make_error (err, 0, "cannot disable OOM killer with cgroupv2");

      ret = write_cgroup_file (dirfd, "memory.oom_control", "1", 1, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }
  if (memory->kernel_tcp_present)
    {
      if (cgroup2)
        return crun_make_error (err, 0, "cannot set kernel TCP with cgroupv2");

      len = snprintf (fmt_buf, sizeof (fmt_buf), "%" PRIu64, memory->kernel_tcp);
      if (UNLIKELY (len >= (int) sizeof (fmt_buf)))
        return crun_make_error (err, 0, "internal error: static buffer too small");
      ret = write_cgroup_file (dirfd, "memory.kmem.tcp.limit_in_bytes", fmt_buf, len, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }
  if (memory->swappiness_present)
    {
      if (cgroup2)
        return crun_make_error (err, 0, "cannot set memory swappiness with cgroupv2");

      len = snprintf (fmt_buf, sizeof (fmt_buf), "%" PRIu64, memory->swappiness);
      if (UNLIKELY (len >= (int) sizeof (fmt_buf)))
        return crun_make_error (err, 0, "internal error: static buffer too small");
      ret = write_cgroup_file (dirfd, "memory.swappiness", fmt_buf, len, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  return 0;
}

int
write_cpu_burst (int cpu_dirfd, bool cgroup2, runtime_spec_schema_config_linux_resources_cpu *cpu,
                 libcrun_error_t *err)
{
  char fmt_buf[32];
  int len;

  if (! cpu->burst_present)
    return 0;

  len = snprintf (fmt_buf, sizeof (fmt_buf), "%" PRIi64, cpu->burst);
  if (UNLIKELY (len >= (int) sizeof (fmt_buf)))
    return crun_make_error (err, 0, "internal error: static buffer too small");
  return write_cgroup_file (cpu_dirfd, cgroup2 ? "cpu.max.burst" : "cpu.cfs_burst_us", fmt_buf, len, err);
}

static int
write_pids_resources (int dirfd, bool cgroup2, runtime_spec_schema_config_linux_resources_pids *pids,
                      libcrun_error_t *err)
{
  if (pids->limit)
    {
      char fmt_buf[32];
      int len;
      int ret;

      len = cg_itoa (fmt_buf, sizeof (fmt_buf), pids->limit, true, err);
      if (UNLIKELY (len < 0))
        return len;

      ret = write_file_and_check_controllers_at (cgroup2, dirfd, "pids.max", NULL, fmt_buf, len, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  return 0;
}

static int
write_cpu_resources (int dirfd_cpu, bool cgroup2, runtime_spec_schema_config_linux_resources_cpu *cpu,
                     libcrun_error_t *err)
{
  int len, period_len;
  int ret;
  char fmt_buf[64];
  int64_t period = -1;
  int64_t quota = -1;
  cleanup_free char *period_str = NULL;

  if (cpu->shares)
    {
      uint32_t val = cpu->shares;

      if (cgroup2)
        val = convert_shares_to_weight (val);

      len = snprintf (fmt_buf, sizeof (fmt_buf), "%u", val);
      if (UNLIKELY (len >= (int) sizeof (fmt_buf)))
        return crun_make_error (err, 0, "internal error: static buffer too small");

      ret = write_file_and_check_controllers_at (cgroup2, dirfd_cpu, cgroup2 ? "cpu.weight" : "cpu.shares",
                                                 NULL, fmt_buf, len, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }
  if (cpu->period)
    {
      if (cgroup2)
        period = cpu->period;
      else
        {
          len = snprintf (fmt_buf, sizeof (fmt_buf), "%" PRIu64, cpu->period);
          if (UNLIKELY (len >= (int) sizeof (fmt_buf)))
            return crun_make_error (err, 0, "internal error: static buffer too small");
          ret = write_cgroup_file (dirfd_cpu, "cpu.cfs_period_us", fmt_buf, len, err);
          if (UNLIKELY (ret < 0))
            {
              /*
                Sometimes when the period to be set is smaller than the current one,
                it is rejected by the kernel (EINVAL) as old_quota/new_period exceeds
                the parent cgroup quota limit. If this happens and the quota is going
                to be set, ignore the error for now and retry after setting the quota.
               */
              if (! cpu->quota || crun_error_get_errno (err) != EINVAL)
                return ret;

              crun_error_release (err);
              period_str = xstrdup (fmt_buf);
              period_len = len;
            }
        }
    }
  if (cpu->quota)
    {
      if (cgroup2)
        quota = cpu->quota;
      else
        {
          len = snprintf (fmt_buf, sizeof (fmt_buf), "%" PRIi64, cpu->quota);
          if (UNLIKELY (len >= (int) sizeof (fmt_buf)))
            return crun_make_error (err, 0, "internal error: static buffer too small");
          ret = write_cgroup_file (dirfd_cpu, "cpu.cfs_quota_us", fmt_buf, len, err);
          if (UNLIKELY (ret < 0))
            return ret;
          if (period_str != NULL)
            {
              ret = write_cgroup_file (dirfd_cpu, "cpu.cfs_period_us", period_str, period_len, err);
              if (UNLIKELY (ret < 0))
                return ret;
            }
        }
    }
  if (cpu->realtime_period)
    {
      if (cgroup2)
        return crun_make_error (err, 0, "realtime period not supported on cgroupv2");
      len = snprintf (fmt_buf, sizeof (fmt_buf), "%" PRIu64, cpu->realtime_period);
      if (UNLIKELY (len >= (int) sizeof (fmt_buf)))
        return crun_make_error (err, 0, "internal error: static buffer too small");
      ret = write_cgroup_file (dirfd_cpu, "cpu.rt_period_us", fmt_buf, len, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }
  if (cpu->realtime_runtime)
    {
      if (cgroup2)
        return crun_make_error (err, 0, "realtime runtime not supported on cgroupv2");
      len = snprintf (fmt_buf, sizeof (fmt_buf), "%" PRIu64, cpu->realtime_runtime);
      if (UNLIKELY (len >= (int) sizeof (fmt_buf)))
        return crun_make_error (err, 0, "internal error: static buffer too small");
      ret = write_cgroup_file (dirfd_cpu, "cpu.rt_runtime_us", fmt_buf, len, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }
  if (cpu->idle_present)
    {
      len = snprintf (fmt_buf, sizeof (fmt_buf), "%" PRIi64, cpu->idle);
      if (UNLIKELY (len >= (int) sizeof (fmt_buf)))
        return crun_make_error (err, 0, "internal error: static buffer too small");
      ret = write_cgroup_file (dirfd_cpu, "cpu.idle", fmt_buf, len, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  if (cgroup2 && (quota > 0 || period > 0))
    {
      if (period < 0)
        period = 100000;
      if (quota < 0)
        len = snprintf (fmt_buf, sizeof (fmt_buf), "max %" PRIi64, period);
      else
        len = snprintf (fmt_buf, sizeof (fmt_buf), "%" PRIi64 " %" PRIi64, quota, period);

      if (UNLIKELY (len >= (int) sizeof (fmt_buf)))
        return crun_make_error (err, 0, "internal error: static buffer too small");

      ret = write_file_and_check_controllers_at (cgroup2, dirfd_cpu, "cpu.max", NULL, fmt_buf, len, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  return write_cpu_burst (dirfd_cpu, cgroup2, cpu, err);
}

int
write_cpuset_resources (int dirfd_cpuset, int cgroup2, runtime_spec_schema_config_linux_resources_cpu *cpu,
                        libcrun_error_t *err)
{
  int ret;

  if (cpu == NULL)
    return 0;

  if (cpu->cpus)
    {
      ret = write_file_and_check_controllers_at (cgroup2, dirfd_cpuset, "cpuset.cpus", "cpus",
                                                 cpu->cpus, strlen (cpu->cpus), err);
      if (UNLIKELY (ret < 0))
        return ret;
    }
  if (cpu->mems)
    {
      ret = write_file_and_check_controllers_at (cgroup2, dirfd_cpuset, "cpuset.mems", "mems", cpu->mems, strlen (cpu->mems),
                                                 err);
      if (UNLIKELY (ret < 0))
        return ret;
    }
  return 0;
}

static int
open_cgroup_subsystem (const char *subsystem, const char *path, libcrun_error_t *err)
{
  cleanup_free char *full_path = NULL;
  int ret, dirfd;

  ret = append_paths (&full_path, err, CGROUP_ROOT, subsystem, path, NULL);
  if (UNLIKELY (ret < 0))
    return ret;

  dirfd = open (full_path, O_DIRECTORY | O_PATH | O_CLOEXEC);
  if (UNLIKELY (dirfd < 0))
    return crun_make_error (err, errno, "open `%s`", full_path);

  return dirfd;
}

int
update_cgroup_v1_resources (runtime_spec_schema_config_linux_resources *resources, const char *path, libcrun_error_t *err)
{
  int ret;

  if (resources->block_io)
    {
      cleanup_close int dirfd_blkio = -1;
      runtime_spec_schema_config_linux_resources_block_io *blkio = resources->block_io;

      dirfd_blkio = open_cgroup_subsystem ("blkio", path, err);
      if (UNLIKELY (dirfd_blkio < 0))
        return dirfd_blkio;

      ret = write_blkio_resources (dirfd_blkio, false, blkio, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  if (resources->network)
    {
      cleanup_close int dirfd_netclass = -1;
      cleanup_close int dirfd_netprio = -1;
      runtime_spec_schema_config_linux_resources_network *network = resources->network;

      dirfd_netclass = open_cgroup_subsystem ("net_cls", path, err);
      if (UNLIKELY (dirfd_netclass < 0))
        return dirfd_netclass;

      dirfd_netprio = open_cgroup_subsystem ("net_prio", path, err);
      if (UNLIKELY (dirfd_netprio < 0))
        return dirfd_netprio;

      ret = write_network_resources (dirfd_netclass, dirfd_netprio, network, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  if (resources->hugepage_limits_len)
    {
      cleanup_close int dirfd_htlb = -1;

      dirfd_htlb = open_cgroup_subsystem ("hugetlb", path, err);
      if (UNLIKELY (dirfd_htlb < 0))
        return dirfd_htlb;

      ret = write_hugetlb_resources (dirfd_htlb, false, resources->hugepage_limits, resources->hugepage_limits_len,
                                     err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  if (resources->devices_len)
    {
      cleanup_close int dirfd_devs = -1;

      dirfd_devs = open_cgroup_subsystem ("devices", path, err);
      if (UNLIKELY (dirfd_devs < 0))
        return dirfd_devs;

      ret = write_devices_resources (dirfd_devs, false, resources->devices, resources->devices_len, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  if (resources->memory)
    {
      cleanup_close int dirfd_mem = -1;

      dirfd_mem = open_cgroup_subsystem ("memory", path, err);
      if (UNLIKELY (dirfd_mem < 0))
        return dirfd_mem;

      ret = write_memory_resources (dirfd_mem, false, resources->memory, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  if (resources->pids)
    {
      cleanup_close int dirfd_pid = -1;

      dirfd_pid = open_cgroup_subsystem ("pids", path, err);
      if (UNLIKELY (dirfd_pid < 0))
        return dirfd_pid;

      ret = write_pids_resources (dirfd_pid, false, resources->pids, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  if (resources->cpu)
    {
      cleanup_close int dirfd_cpu = -1;
      cleanup_close int dirfd_cpuset = -1;

      dirfd_cpu = open_cgroup_subsystem ("cpu", path, err);
      if (UNLIKELY (dirfd_cpu < 0))
        return dirfd_cpu;

      ret = write_cpu_resources (dirfd_cpu, false, resources->cpu, err);
      if (UNLIKELY (ret < 0))
        return ret;

      if (resources->cpu->cpus == NULL && resources->cpu->mems == NULL)
        return 0;

      dirfd_cpuset = open_cgroup_subsystem ("cpuset", path, err);
      if (UNLIKELY (dirfd_cpuset < 0))
        return dirfd_cpuset;

      ret = write_cpuset_resources (dirfd_cpuset, false, resources->cpu, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  if (resources->unified && resources->unified->len > 0)
    return crun_make_error (err, 0, "invalid configuration: cannot use unified on cgroup v1");

  return 0;
}

static int
write_unified_resources (int cgroup_dirfd, runtime_spec_schema_config_linux_resources *resources, libcrun_error_t *err)
{
  size_t i;
  int ret;

  for (i = 0; i < resources->unified->len; i++)
    {
      cleanup_close int fd = -1;
      cleanup_free char *value = NULL;
      char *saveptr = NULL;
      char *line;

      if (strchr (resources->unified->keys[i], '/'))
        return crun_make_error (err, 0, "key `%s` must be a file name without any slash", resources->unified->keys[i]);

      if (is_empty_string (resources->unified->values[i]))
        continue;

      value = xstrdup (resources->unified->values[i]);

      fd = open_file_and_check_controllers_at (true, cgroup_dirfd, resources->unified->keys[i], O_WRONLY, err);
      if (UNLIKELY (fd < 0))
        return fd;

      for (line = strtok_r (value, "\n", &saveptr); line; line = strtok_r (NULL, "\n", &saveptr))
        {
          ret = TEMP_FAILURE_RETRY (write (fd, line, strlen (line)));
          if (UNLIKELY (ret < 0))
            return crun_make_error (err, errno, "write to `%s`", resources->unified->keys[i]);
        }
    }

  return 0;
}

static int
update_cgroup_v2_resources (runtime_spec_schema_config_linux_resources *resources, const char *path, bool need_bpf_dev, libcrun_error_t *err)
{
  cleanup_free char *cgroup_path = NULL;
  cleanup_close int cgroup_dirfd = -1;
  int ret;

  if (resources->network)
    return crun_make_error (err, 0, "network limits not supported on cgroupv2");

  ret = append_paths (&cgroup_path, err, CGROUP_ROOT, path, NULL);
  if (UNLIKELY (ret < 0))
    return ret;

  cgroup_dirfd = open (cgroup_path, O_DIRECTORY | O_CLOEXEC);
  if (UNLIKELY (cgroup_dirfd < 0))
    return crun_make_error (err, errno, "open `%s`", cgroup_path);

  if (need_bpf_dev && resources->devices_len)
    {
      ret = write_devices_resources (cgroup_dirfd, true, resources->devices, resources->devices_len, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  if (resources->memory)
    {
      ret = write_memory_resources (cgroup_dirfd, true, resources->memory, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }
  if (resources->pids)
    {
      ret = write_pids_resources (cgroup_dirfd, true, resources->pids, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }
  if (resources->cpu)
    {
      ret = write_cpu_resources (cgroup_dirfd, true, resources->cpu, err);
      if (UNLIKELY (ret < 0))
        return ret;

      ret = write_cpuset_resources (cgroup_dirfd, true, resources->cpu, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }
  if (resources->block_io)
    {
      ret = write_blkio_resources (cgroup_dirfd, true, resources->block_io, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  if (resources->hugepage_limits_len)
    {
      ret = write_hugetlb_resources (cgroup_dirfd, true, resources->hugepage_limits, resources->hugepage_limits_len,
                                     err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  /* Write unified resources if any.  They have higher precedence and override any previous setting.  */
  if (resources->unified)
    {
      ret = write_unified_resources (cgroup_dirfd, resources, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  return 0;
}

int
update_cgroup_resources (const char *path,
                         const char *state_root,
                         runtime_spec_schema_config_linux_resources *resources,
                         bool need_bpf_dev,
                         libcrun_error_t *err)
{
  int cgroup_mode;

  (void) state_root;

  cgroup_mode = libcrun_get_cgroup_mode (err);
  if (UNLIKELY (cgroup_mode < 0))
    return cgroup_mode;

  if (path == NULL)
    {
      size_t i;

      if (resources->block_io || resources->network || resources->hugepage_limits_len || resources->memory
          || resources->pids || resources->cpu)
        return crun_make_error (err, 0, "cannot set limits without cgroups");

      for (i = 0; i < resources->devices_len; i++)
        {
          int rwm;

          rwm = is_rwm (resources->devices[i]->access, err);
          if (UNLIKELY (rwm < 0))
            return rwm;

          if (rwm == 0)
            return crun_make_error (err, 0, "cannot set limits without cgroups");
        }

      return 0;
    }
  switch (cgroup_mode)
    {
    case CGROUP_MODE_UNIFIED:
      return update_cgroup_v2_resources (resources, path, need_bpf_dev, err);

    case CGROUP_MODE_LEGACY:
    case CGROUP_MODE_HYBRID:
      return update_cgroup_v1_resources (resources, path, err);

    default:
      return crun_make_error (err, 0, "invalid cgroup mode `%d`", cgroup_mode);
    }
}
