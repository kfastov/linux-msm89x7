/*
 * Copyright (c) 2013 Eugene Krasnikov <k.eugene.e@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include "wcn36xx.h"
#include "debug.h"
#include "pmc.h"
#include "firmware.h"

#ifdef CONFIG_WCN36XX_DEBUGFS

static ssize_t read_file_bool_bmps(struct file *file, char __user *user_buf,
				   size_t count, loff_t *ppos)
{
	struct wcn36xx *wcn = file->private_data;
	struct wcn36xx_vif *vif_priv = NULL;
	struct ieee80211_vif *vif = NULL;
	char buf[3];

	list_for_each_entry(vif_priv, &wcn->vif_list, list) {
			vif = wcn36xx_priv_to_vif(vif_priv);
			if (NL80211_IFTYPE_STATION == vif->type) {
				if (vif_priv->pw_state == WCN36XX_BMPS)
					buf[0] = '1';
				else
					buf[0] = '0';
				break;
			}
	}
	buf[1] = '\n';
	buf[2] = 0x00;

	return simple_read_from_buffer(user_buf, count, ppos, buf, 2);
}

static ssize_t write_file_bool_bmps(struct file *file,
				    const char __user *user_buf,
				    size_t count, loff_t *ppos)
{
	struct wcn36xx *wcn = file->private_data;
	struct wcn36xx_vif *vif_priv = NULL;
	struct ieee80211_vif *vif = NULL;

	char buf[32];
	int buf_size;

	buf_size = min(count, (sizeof(buf)-1));
	if (copy_from_user(buf, user_buf, buf_size))
		return -EFAULT;

	switch (buf[0]) {
	case 'y':
	case 'Y':
	case '1':
		list_for_each_entry(vif_priv, &wcn->vif_list, list) {
			vif = wcn36xx_priv_to_vif(vif_priv);
			if (NL80211_IFTYPE_STATION == vif->type) {
				wcn36xx_enable_keep_alive_null_packet(wcn, vif);
				wcn36xx_pmc_enter_bmps_state(wcn, vif);
			}
		}
		break;
	case 'n':
	case 'N':
	case '0':
		list_for_each_entry(vif_priv, &wcn->vif_list, list) {
			vif = wcn36xx_priv_to_vif(vif_priv);
			if (NL80211_IFTYPE_STATION == vif->type)
				wcn36xx_pmc_exit_bmps_state(wcn, vif);
		}
		break;
	}

	return count;
}

static const struct file_operations fops_wcn36xx_bmps = {
	.open = simple_open,
	.read  =       read_file_bool_bmps,
	.write =       write_file_bool_bmps,
};

static ssize_t write_file_dump(struct file *file,
				    const char __user *user_buf,
				    size_t count, loff_t *ppos)
{
	struct wcn36xx *wcn = file->private_data;
	char buf[255], *tmp;
	int buf_size;
	u32 arg[WCN36xx_MAX_DUMP_ARGS];
	int i;

	memset(buf, 0, sizeof(buf));
	memset(arg, 0, sizeof(arg));

	buf_size = min(count, (sizeof(buf) - 1));
	if (copy_from_user(buf, user_buf, buf_size))
		return -EFAULT;

	tmp = buf;

	for (i = 0; i < WCN36xx_MAX_DUMP_ARGS; i++) {
		char *begin;
		begin = strsep(&tmp, " ");
		if (begin == NULL)
			break;

		if (kstrtos32(begin, 0, &arg[i]) != 0)
			break;
	}

	wcn36xx_info("DUMP args is %d %d %d %d %d\n", arg[0], arg[1], arg[2],
		     arg[3], arg[4]);
	wcn36xx_smd_dump_cmd_req(wcn, arg[0], arg[1], arg[2], arg[3], arg[4]);

	return count;
}

static ssize_t read_file_dump(struct file *file, char __user *user_buf,
			      size_t count, loff_t *ppos)
{
	struct wcn36xx *wcn = file->private_data;

	if (!wcn->dump_rsp_len)
		return 0;

	return simple_read_from_buffer(user_buf, count, ppos,
				       wcn->dump_rsp, wcn->dump_rsp_len);
}

static const struct file_operations fops_wcn36xx_dump = {
	.open = simple_open,
	.write =       write_file_dump,
	.read  =       read_file_dump,
};

/*
 * Ask the firmware which values of enum wcn36xx_hal_sys_mode it accepts in
 * INIT_SCAN_REQ.  HAL_SYS_MODE_PROMISC is declared in hal.h and has never been
 * sent by any published driver - in Qualcomm's prima the equivalent constant
 * appears only inside the function that translates it - so whether this
 * firmware implements it cannot be answered from source.
 *
 * wcn36xx_smd_init_scan() returns the firmware's own status word, so the answer
 * is one number per mode.
 *
 * Values above the declared enum are deliberately allowed through.  A firmware
 * that does not validate the field at all would return success for every legal
 * mode too, and then "PROMISC accepted" would mean nothing; writing 9 is the
 * negative control that tells the two apart.
 *
 *   echo 3            > sysmode_probe   enter mode 3, then leave it again
 *   echo "3 hold"     > sysmode_probe   enter and stay, on the current channel
 *   echo "3 hold 11"  > sysmode_probe   enter and stay, on channel 11
 *   echo finish       > sysmode_probe   leave whatever is being held
 *
 * Holding a mode is what makes a behavioural test possible: a status word says
 * the firmware accepted a request, only arriving frames say it did anything.
 *
 * Holding sends START_SCAN as well as INIT_SCAN, because INIT_SCAN alone was
 * measured to do nothing at all: held in HAL_SYS_MODE_SCAN - the one mode this
 * firmware demonstrably honours during ordinary software scans - not one
 * foreign frame arrived.  INIT_SCAN announces an intent; START_SCAN is what
 * acts on it.
 *
 * The channel defaults to the operating one, so the radio does not move and
 * the link stays up.  Passing any other channel will take the radio off
 * channel, which on this device means losing the route in.
 */
#define WCN36XX_SYSMODE_PROBE_MAX 16

static const char * const wcn36xx_sys_mode_name[] = {
	"NORMAL", "LEARN", "SCAN", "PROMISC",
	"SUSPEND_LINK", "ROAM_SCAN", "ROAM_SUSPEND_LINK",
};

static int wcn36xx_sysmode_status[WCN36XX_SYSMODE_PROBE_MAX];
static bool wcn36xx_sysmode_tried[WCN36XX_SYSMODE_PROBE_MAX];
static int wcn36xx_sysmode_held = -1;
static u8 wcn36xx_sysmode_held_ch;

static struct ieee80211_vif *wcn36xx_first_vif(struct wcn36xx *wcn)
{
	struct ieee80211_vif *vif = NULL;
	struct wcn36xx_vif *tmp;

	mutex_lock(&wcn->conf_mutex);
	tmp = list_first_entry_or_null(&wcn->vif_list, struct wcn36xx_vif, list);
	if (tmp)
		vif = wcn36xx_priv_to_vif(tmp);
	mutex_unlock(&wcn->conf_mutex);

	return vif;
}

static ssize_t write_file_sysmode_probe(struct file *file,
					const char __user *user_buf,
					size_t count, loff_t *ppos)
{
	struct wcn36xx *wcn = file->private_data;
	struct ieee80211_vif *vif;
	char buf[32], *p, *tok, *c;
	bool hold;
	u32 mode;
	u8 ch = 0;
	int ret;

	if (count >= sizeof(buf))
		return -EINVAL;
	memset(buf, 0, sizeof(buf));
	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	vif = wcn36xx_first_vif(wcn);
	if (!vif)
		return -ENODEV;

	p = strim(buf);

	if (!strcmp(p, "finish")) {
		int end;

		if (wcn36xx_sysmode_held < 0)
			return -EINVAL;
		end = wcn36xx_smd_end_scan(wcn, wcn36xx_sysmode_held_ch);
		ret = wcn36xx_smd_finish_scan(wcn, wcn36xx_sysmode_held, vif);
		wcn36xx_info("sysmode_probe: released mode %d ch %u -> end %d finish %d\n",
			     wcn36xx_sysmode_held, wcn36xx_sysmode_held_ch,
			     end, ret);
		wcn36xx_sysmode_held = -1;
		return count;
	}

	if (wcn36xx_sysmode_held >= 0) {
		wcn36xx_warn("sysmode_probe: mode %d still held, write finish first\n",
			     wcn36xx_sysmode_held);
		return -EBUSY;
	}

	tok = strsep(&p, " \t");
	if (!tok || kstrtou32(tok, 0, &mode))
		return -EINVAL;
	if (mode >= WCN36XX_SYSMODE_PROBE_MAX)
		return -ERANGE;
	hold = p && strstr(p, "hold");
	if (hold) {
		c = strstr(p, "hold") + 4;
		while (*c == ' ' || *c == '\t')
			c++;
		if (!*c || kstrtou8(c, 0, &ch))
			ch = 0;
	}
	if (!ch)
		ch = WCN36XX_HW_CHANNEL(wcn);

	if (wcn->sw_scan || wcn->sw_scan_init) {
		wcn36xx_warn("sysmode_probe: a scan is in progress\n");
		return -EBUSY;
	}

	ret = wcn36xx_smd_init_scan(wcn, mode, vif);
	wcn36xx_sysmode_status[mode] = ret;
	wcn36xx_sysmode_tried[mode] = true;
	wcn36xx_info("sysmode_probe: mode %u -> %d%s\n", mode, ret,
		     (!ret && hold) ? " (held)" : "");

	if (!ret) {
		if (hold) {
			int start = wcn36xx_smd_start_scan(wcn, ch);

			wcn36xx_info("sysmode_probe: start_scan ch %u -> %d\n",
				     ch, start);
			wcn36xx_sysmode_held = mode;
			wcn36xx_sysmode_held_ch = ch;
		} else {
			wcn36xx_smd_finish_scan(wcn, mode, vif);
		}
	}

	return count;
}

static ssize_t read_file_sysmode_probe(struct file *file, char __user *user_buf,
				       size_t count, loff_t *ppos)
{
	char buf[1024];
	size_t len = 0;
	int i;

	len += scnprintf(buf + len, sizeof(buf) - len,
			 "mode  name                   status\n");
	for (i = 0; i < WCN36XX_SYSMODE_PROBE_MAX; i++) {
		const char *name;

		if (!wcn36xx_sysmode_tried[i])
			continue;

		name = i < ARRAY_SIZE(wcn36xx_sys_mode_name) ?
			wcn36xx_sys_mode_name[i] : "(not a declared mode)";

		len += scnprintf(buf + len, sizeof(buf) - len,
				 "%4d  %-21s  %4d  %s%s\n", i, name,
				 wcn36xx_sysmode_status[i],
				 wcn36xx_sysmode_status[i] ? "refused" : "accepted",
				 i == wcn36xx_sysmode_held ? "  [HELD]" : "");
	}
	if (wcn36xx_sysmode_held >= 0)
		len += scnprintf(buf + len, sizeof(buf) - len,
				 "held on channel %u\n", wcn36xx_sysmode_held_ch);
	if (len < 60)
		len += scnprintf(buf + len, sizeof(buf) - len,
				 "  (nothing tried yet)\n");

	return simple_read_from_buffer(user_buf, count, ppos, buf, len);
}

static const struct file_operations fops_wcn36xx_sysmode_probe = {
	.open  = simple_open,
	.write = write_file_sysmode_probe,
	.read  = read_file_sysmode_probe,
};

static ssize_t read_file_firmware_feature_caps(struct file *file,
					       char __user *user_buf,
					       size_t count, loff_t *ppos)
{
	struct wcn36xx *wcn = file->private_data;
	size_t len = 0, buf_len = 2048;
	char *buf;
	int i;
	int ret;

	buf = kzalloc(buf_len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mutex_lock(&wcn->hal_mutex);
	for (i = 0; i < MAX_FEATURE_SUPPORTED; i++) {
		if (wcn36xx_firmware_get_feat_caps(wcn->fw_feat_caps, i)) {
			len += scnprintf(buf + len, buf_len - len, "%s\n",
					 wcn36xx_firmware_get_cap_name(i));
		}
		if (len >= buf_len)
			break;
	}
	mutex_unlock(&wcn->hal_mutex);

	ret = simple_read_from_buffer(user_buf, count, ppos, buf, len);
	kfree(buf);

	return ret;
}

static const struct file_operations fops_wcn36xx_firmware_feat_caps = {
	.open = simple_open,
	.read = read_file_firmware_feature_caps,
};

#define ADD_FILE(name, mode, fop, priv_data)		\
	do {							\
		struct dentry *d;				\
		d = debugfs_create_file(__stringify(name),	\
					mode, dfs->rootdir,	\
					priv_data, fop);	\
		dfs->file_##name.dentry = d;			\
		if (IS_ERR(d)) {				\
			wcn36xx_warn("Create the debugfs entry failed");\
			dfs->file_##name.dentry = NULL;		\
		}						\
	} while (0)


void wcn36xx_debugfs_init(struct wcn36xx *wcn)
{
	struct wcn36xx_dfs_entry *dfs = &wcn->dfs;

	dfs->rootdir = debugfs_create_dir(KBUILD_MODNAME,
					  wcn->hw->wiphy->debugfsdir);
	if (IS_ERR(dfs->rootdir)) {
		wcn36xx_warn("Create the debugfs failed\n");
		dfs->rootdir = NULL;
	}

	ADD_FILE(bmps_switcher, 0600, &fops_wcn36xx_bmps, wcn);
	ADD_FILE(dump, 0600, &fops_wcn36xx_dump, wcn);
	ADD_FILE(firmware_feat_caps, 0200,
		 &fops_wcn36xx_firmware_feat_caps, wcn);
	ADD_FILE(sysmode_probe, 0600, &fops_wcn36xx_sysmode_probe, wcn);
}

void wcn36xx_debugfs_exit(struct wcn36xx *wcn)
{
	struct wcn36xx_dfs_entry *dfs = &wcn->dfs;
	debugfs_remove_recursive(dfs->rootdir);
}

#endif /* CONFIG_WCN36XX_DEBUGFS */
