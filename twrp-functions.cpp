/*
	Copyright 2012 bigbiff/Dees_Troy TeamWin
	This file is part of TWRP/TeamWin Recovery Project.

	TWRP is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	TWRP is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with TWRP.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <unistd.h>
#include <vector>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include "twrp-functions.hpp"
#include "twcommon.h"
#ifndef BUILD_TWRPTAR_MAIN
#include "data.hpp"
#include "partitions.hpp"
#include "variables.h"
#include "bootloader.h"
#ifdef ANDROID_RB_POWEROFF
	#include "cutils/android_reboot.h"
#endif
#endif // ndef BUILD_TWRPTAR_MAIN
#ifndef TW_EXCLUDE_ENCRYPTED_BACKUPS
	#include "openaes/inc/oaes_lib.h"
#endif

extern "C" {
	#include "libcrecovery/common.h"
	#include "minuitwrp/minui.h"
}

#include "gui/objects.hpp"

#ifdef HAVE_SELINUX
#include <selinux/selinux.h>
#include <selinux/label.h>
#include <selinux/android.h>
#include <selinux/label.h>
#endif

/* Execute a command */
int TWFunc::Exec_Cmd(const string& cmd, string &result) {
	FILE* exec;
	char buffer[130];
	int ret = 0;
	exec = __popen(cmd.c_str(), "r");
	if (!exec) return -1;
	while(!feof(exec)) {
		memset(&buffer, 0, sizeof(buffer));
		if (fgets(buffer, 128, exec) != NULL) {
			buffer[128] = '\n';
			buffer[129] = NULL;
			result += buffer;
		}
	}
	ret = __pclose(exec);
	return ret;
}

int TWFunc::Exec_Cmd(const string& cmd) {
	pid_t pid;
	int status;
	switch(pid = fork())
	{
		case -1:
			LOGERR("Exec_Cmd(): vfork failed: %d!\n", errno);
			return -1;
		case 0: // child
			execl("/sbin/sh", "sh", "-c", cmd.c_str(), NULL);
			_exit(127);
			break;
		default:
		{
			if (TWFunc::Wait_For_Child(pid, &status, cmd) != 0)
				return -1;
			else
				return 0;
		}
	}
}

int TWFunc::Exec_Cmd_Show_Output(const string& cmd) {
	FILE* exec;
	char buffer[130];
	int ret = 0;
	exec = __popen(cmd.c_str(), "r");

	if (!exec)
		return -1;

	while(!feof(exec)) {
		memset(buffer, 0, sizeof(buffer));
		if (fgets(buffer, 128, exec) != NULL) {
			buffer[128] = '\n';
			buffer[129] = NULL;
			gui_print(buffer);
		}
	}
	ret = __pclose(exec);
	return ret;
}

// Returns "file.name" from a full /path/to/file.name
string TWFunc::Get_Filename(string Path) {
	size_t pos = Path.find_last_of("/");
	if (pos != string::npos) {
		string Filename;
		Filename = Path.substr(pos + 1, Path.size() - pos - 1);
		return Filename;
	} else
		return Path;
}

// Returns "/path/to/" from a full /path/to/file.name
string TWFunc::Get_Path(string Path) {
	size_t pos = Path.find_last_of("/");
	if (pos != string::npos) {
		string Pathonly;
		Pathonly = Path.substr(0, pos + 1);
		return Pathonly;
	} else
		return Path;
}

int TWFunc::Wait_For_Child(pid_t pid, int *status, string Child_Name) {
	pid_t rc_pid;

	rc_pid = waitpid(pid, status, 0);
	if (rc_pid > 0) {
		if (WEXITSTATUS(*status) == 0)
			LOGINFO("%s process ended with RC=%d\n", Child_Name.c_str(), WEXITSTATUS(*status)); // Success
		else if (WIFSIGNALED(*status)) {
			LOGINFO("%s process ended with signal: %d\n", Child_Name.c_str(), WTERMSIG(*status)); // Seg fault or some other non-graceful termination
			return -1;
		} else if (WEXITSTATUS(*status) != 0) {
			LOGINFO("%s process ended with ERROR=%d\n", Child_Name.c_str(), WEXITSTATUS(*status)); // Graceful exit, but there was an error
			return -1;
		}
	} else { // no PID returned
		if (errno == ECHILD)
			LOGINFO("%s no child process exist\n", Child_Name.c_str());
		else {
			LOGINFO("%s Unexpected error\n", Child_Name.c_str());
			return -1;
		}
	}
	return 0;
}

bool TWFunc::Path_Exists(string Path) {
	// Check to see if the Path exists
	struct stat st;
	if (stat(Path.c_str(), &st) != 0)
		return false;
	else
		return true;
}

int TWFunc::Get_File_Type(string fn) {
	string::size_type i = 0;
	int firstbyte = 0, secondbyte = 0;
	char header[3];

	ifstream f;
	f.open(fn.c_str(), ios::in | ios::binary);
	f.get(header, 3);
	f.close();
	firstbyte = header[i] & 0xff;
	secondbyte = header[++i] & 0xff;

	if (firstbyte == 0x1f && secondbyte == 0x8b)
		return 1; // Compressed
	else if (firstbyte == 0x4f && secondbyte == 0x41)
		return 2; // Encrypted
	else
		return 0; // Unknown

	return 0;
}

int TWFunc::Try_Decrypting_File(string fn, string password) {
#ifndef TW_EXCLUDE_ENCRYPTED_BACKUPS
	OAES_CTX * ctx = NULL;
	uint8_t _key_data[32] = "";
	FILE *f;
	uint8_t buffer[4096];
	uint8_t *buffer_out = NULL;
	uint8_t *ptr = NULL;
	size_t read_len = 0, out_len = 0;
	int firstbyte = 0, secondbyte = 0, key_len;
	size_t _j = 0;
	size_t _key_data_len = 0;

	// mostly kanged from OpenAES oaes.c
	for( _j = 0; _j < 32; _j++ )
		_key_data[_j] = _j + 1;
	_key_data_len = password.size();
	if( 16 >= _key_data_len )
		_key_data_len = 16;
	else if( 24 >= _key_data_len )
		_key_data_len = 24;
	else
	_key_data_len = 32;
	memcpy(_key_data, password.c_str(), password.size());

	ctx = oaes_alloc();
	if (ctx == NULL) {
		LOGERR("Failed to allocate OAES\n");
		return -1;
	}

	oaes_key_import_data(ctx, _key_data, _key_data_len);

	f = fopen(fn.c_str(), "rb");
	if (f == NULL) {
		LOGERR("Failed to open '%s' to try decrypt\n", fn.c_str());
		return -1;
	}
	read_len = fread(buffer, sizeof(uint8_t), 4096, f);
	if (read_len <= 0) {
		LOGERR("Read size during try decrypt failed\n");
		fclose(f);
		return -1;
	}
	if (oaes_decrypt(ctx, buffer, read_len, NULL, &out_len) != OAES_RET_SUCCESS) {
		LOGERR("Error: Failed to retrieve required buffer size for trying decryption.\n");
		fclose(f);
		return -1;
	}
	buffer_out = (uint8_t *) calloc(out_len, sizeof(char));
	if (buffer_out == NULL) {
		LOGERR("Failed to allocate output buffer for try decrypt.\n");
		fclose(f);
		return -1;
	}
	if (oaes_decrypt(ctx, buffer, read_len, buffer_out, &out_len) != OAES_RET_SUCCESS) {
		LOGERR("Failed to decrypt file '%s'\n", fn.c_str());
		fclose(f);
		free(buffer_out);
		return 0;
	}
	fclose(f);
	if (out_len < 2) {
		LOGINFO("Successfully decrypted '%s' but read length %i too small.\n", fn.c_str(), out_len);
		free(buffer_out);
		return 1; // Decrypted successfully
	}
	ptr = buffer_out;
	firstbyte = *ptr & 0xff;
	ptr++;
	secondbyte = *ptr & 0xff;
	if (firstbyte == 0x1f && secondbyte == 0x8b) {
		LOGINFO("Successfully decrypted '%s' and file is compressed.\n", fn.c_str());
		free(buffer_out);
		return 3; // Compressed
	}
	if (out_len >= 262) {
		ptr = buffer_out + 257;
		if (strncmp((char*)ptr, "ustar", 5) == 0) {
			LOGINFO("Successfully decrypted '%s' and file is tar format.\n", fn.c_str());
			free(buffer_out);
			return 2; // Tar
		}
	}
	free(buffer_out);
	LOGINFO("No errors decrypting '%s' but no known file format.\n", fn.c_str());
	return 1; // Decrypted successfully
#else
	LOGERR("Encrypted backup support not included.\n");
	return -1;
#endif
}

unsigned long TWFunc::Get_File_Size(string Path) {
	struct stat st;

	if (stat(Path.c_str(), &st) != 0)
		return 0;
	return st.st_size;
}

std::string TWFunc::Remove_Trailing_Slashes(const std::string& path, bool leaveLast)
{
	std::string res;
	size_t last_idx = 0, idx = 0;

	while(last_idx != std::string::npos)
	{
		if(last_idx != 0)
			res += '/';

		idx = path.find_first_of('/', last_idx);
		if(idx == std::string::npos) {
			res += path.substr(last_idx, idx);
			break;
		}

		res += path.substr(last_idx, idx-last_idx);
		last_idx = path.find_first_not_of('/', idx);
	}

	if(leaveLast)
		res += '/';
	return res;
}

#ifndef BUILD_TWRPTAR_MAIN

// Returns "/path" from a full /path/to/file.name
string TWFunc::Get_Root_Path(string Path) {
	string Local_Path = Path;

	// Make sure that we have a leading slash
	if (Local_Path.substr(0, 1) != "/")
		Local_Path = "/" + Local_Path;

	// Trim the path to get the root path only
	size_t position = Local_Path.find("/", 2);
	if (position != string::npos) {
		Local_Path.resize(position);
	}
	return Local_Path;
}

void TWFunc::install_htc_dumlock(void) {
	int need_libs = 0;

	if (!PartitionManager.Mount_By_Path("/system", true))
		return;

	if (!PartitionManager.Mount_By_Path("/data", true))
		return;

	gui_print("Installing HTC Dumlock to system...\n");
	copy_file("/res/htcd/htcdumlocksys", "/system/bin/htcdumlock", 0755);
	if (!Path_Exists("/system/bin/flash_image")) {
		gui_print("Installing flash_image...\n");
		copy_file("/res/htcd/flash_imagesys", "/system/bin/flash_image", 0755);
		need_libs = 1;
	} else
		gui_print("flash_image is already installed, skipping...\n");
	if (!Path_Exists("/system/bin/dump_image")) {
		gui_print("Installing dump_image...\n");
		copy_file("/res/htcd/dump_imagesys", "/system/bin/dump_image", 0755);
		need_libs = 1;
	} else
		gui_print("dump_image is already installed, skipping...\n");
	if (need_libs) {
		gui_print("Installing libs needed for flash_image and dump_image...\n");
		copy_file("/res/htcd/libbmlutils.so", "/system/lib/libbmlutils.so", 0755);
		copy_file("/res/htcd/libflashutils.so", "/system/lib/libflashutils.so", 0755);
		copy_file("/res/htcd/libmmcutils.so", "/system/lib/libmmcutils.so", 0755);
		copy_file("/res/htcd/libmtdutils.so", "/system/lib/libmtdutils.so", 0755);
	}
	gui_print("Installing HTC Dumlock app...\n");
	mkdir("/data/app", 0777);
	unlink("/data/app/com.teamwin.htcdumlock*");
	copy_file("/res/htcd/HTCDumlock.apk", "/data/app/com.teamwin.htcdumlock.apk", 0777);
	sync();
	gui_print("HTC Dumlock is installed.\n");
}

void TWFunc::htc_dumlock_restore_original_boot(void) {
	if (!PartitionManager.Mount_By_Path("/sdcard", true))
		return;

	gui_print("Restoring original boot...\n");
	Exec_Cmd("htcdumlock restore");
	gui_print("Original boot restored.\n");
}

void TWFunc::htc_dumlock_reflash_recovery_to_boot(void) {
	if (!PartitionManager.Mount_By_Path("/sdcard", true))
		return;
	gui_print("Reflashing recovery to boot...\n");
	Exec_Cmd("htcdumlock recovery noreboot");
	gui_print("Recovery is flashed to boot.\n");
}

int TWFunc::Recursive_Mkdir(string Path) {
	string pathCpy = Path;
	string wholePath;
	size_t pos = pathCpy.find("/", 2);

	while (pos != string::npos)
	{
		wholePath = pathCpy.substr(0, pos);
		if (mkdir(wholePath.c_str(), 0777) && errno != EEXIST) {
			LOGERR("Unable to create folder: %s  (errno=%d)\n", wholePath.c_str(), errno);
			return false;
		}

		pos = pathCpy.find("/", pos + 1);
	}
	if (mkdir(wholePath.c_str(), 0777) && errno != EEXIST)
		return false;
	return true;
}

void TWFunc::GUI_Operation_Text(string Read_Value, string Default_Text) {
	string Display_Text;

	DataManager::GetValue(Read_Value, Display_Text);
	if (Display_Text.empty())
		Display_Text = Default_Text;

	DataManager::SetValue("tw_operation", Display_Text);
	DataManager::SetValue("tw_partition", "");
}

void TWFunc::GUI_Operation_Text(string Read_Value, string Partition_Name, string Default_Text) {
	string Display_Text;

	DataManager::GetValue(Read_Value, Display_Text);
	if (Display_Text.empty())
		Display_Text = Default_Text;

	DataManager::SetValue("tw_operation", Display_Text);
	DataManager::SetValue("tw_partition", Partition_Name);
}

void TWFunc::Copy_Log(string Source, string Destination) {
	PartitionManager.Mount_By_Path(Destination, false);
	FILE *destination_log = fopen(Destination.c_str(), "a");
	if (destination_log == NULL) {
		LOGERR("TWFunc::Copy_Log -- Can't open destination log file: '%s'\n", Destination.c_str());
	} else {
		FILE *source_log = fopen(Source.c_str(), "r");
		if (source_log != NULL) {
			fseek(source_log, Log_Offset, SEEK_SET);
			char buffer[4096];
			while (fgets(buffer, sizeof(buffer), source_log))
				fputs(buffer, destination_log); // Buffered write of log file
			Log_Offset = ftell(source_log);
			fflush(source_log);
			fclose(source_log);
		}
		fflush(destination_log);
		fclose(destination_log);
	}
}

void TWFunc::Update_Log_File(void) {
	// Copy logs to cache so the system can find out what happened.
	if (PartitionManager.Mount_By_Path("/cache", false)) {
		if (!TWFunc::Path_Exists("/cache/recovery/.")) {
			LOGINFO("Recreating /cache/recovery folder.\n");
			if (mkdir("/cache/recovery", S_IRWXU | S_IRWXG | S_IWGRP | S_IXGRP) != 0)
				LOGINFO("Unable to create /cache/recovery folder.\n");
		}
		Copy_Log(TMP_LOG_FILE, "/cache/recovery/log");
		copy_file("/cache/recovery/log", "/cache/recovery/last_log", 600);
		chown("/cache/recovery/log", 1000, 1000);
		chmod("/cache/recovery/log", 0600);
		chmod("/cache/recovery/last_log", 0640);
	} else {
		LOGINFO("Failed to mount /cache for TWFunc::Update_Log_File\n");
	}

	// Reset bootloader message
	TWPartition* Part = PartitionManager.Find_Partition_By_Path("/misc");
	if (Part != NULL) {
		struct bootloader_message boot;
		memset(&boot, 0, sizeof(boot));
		if (Part->Current_File_System == "mtd") {
			if (set_bootloader_message_mtd_name(&boot, Part->MTD_Name.c_str()) != 0)
				LOGERR("Unable to set MTD bootloader message.\n");
		} else if (Part->Current_File_System == "emmc") {
			if (set_bootloader_message_block_name(&boot, Part->Actual_Block_Device.c_str()) != 0)
				LOGERR("Unable to set emmc bootloader message.\n");
		} else {
			LOGERR("Unknown file system for /misc: '%s'\n", Part->Current_File_System.c_str());
		}
	}

	if (PartitionManager.Mount_By_Path("/cache", true)) {
		if (unlink("/cache/recovery/command") && errno != ENOENT) {
			LOGINFO("Can't unlink %s\n", "/cache/recovery/command");
		}
	}

	sync();
}

void TWFunc::Update_Intent_File(string Intent) {
	if (PartitionManager.Mount_By_Path("/cache", false) && !Intent.empty()) {
		TWFunc::write_file("/cache/recovery/intent", Intent);
	}
}

// reboot: Reboot the system. Return -1 on error, no return on success
int TWFunc::tw_reboot(RebootCommand command)
{
	// Always force a sync before we reboot
	sync();

	switch (command) {
		case rb_current:
		case rb_system:
			Update_Log_File();
			Update_Intent_File("s");
			sync();
			check_and_run_script("/sbin/rebootsystem.sh", "reboot system");
			return reboot(RB_AUTOBOOT);
		case rb_recovery:
			check_and_run_script("/sbin/rebootrecovery.sh", "reboot recovery");
			return __reboot(LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART2, (void*) "recovery");
		case rb_bootloader:
			check_and_run_script("/sbin/rebootbootloader.sh", "reboot bootloader");
			return __reboot(LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART2, (void*) "bootloader");
		case rb_poweroff:
			check_and_run_script("/sbin/poweroff.sh", "power off");
#ifdef ANDROID_RB_POWEROFF
			android_reboot(ANDROID_RB_POWEROFF, 0, 0);
#endif
			return reboot(RB_POWER_OFF);
		case rb_download:
			check_and_run_script("/sbin/rebootdownload.sh", "reboot download");
			return __reboot(LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART2, (void*) "download");
		default:
			return -1;
	}
	return -1;
}

void TWFunc::check_and_run_script(const char* script_file, const char* display_name)
{
	// Check for and run startup script if script exists
	struct stat st;
	if (stat(script_file, &st) == 0) {
		gui_print("Running %s script...\n", display_name);
		chmod(script_file, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
		TWFunc::Exec_Cmd(script_file);
		gui_print("\nFinished running %s script.\n", display_name);
	}
}

int TWFunc::removeDir(const string path, bool skipParent) {
	DIR *d = opendir(path.c_str());
	int r = 0;
	string new_path;

	if (d == NULL) {
		LOGERR("Error opening '%s'\n", path.c_str());
		return -1;
	}

	if (d) {
		struct dirent *p;
		while (!r && (p = readdir(d))) {
			if (!strcmp(p->d_name, ".") || !strcmp(p->d_name, ".."))
				continue;
			new_path = path + "/";
			new_path.append(p->d_name);
			if (p->d_type == DT_DIR) {
				r = removeDir(new_path, true);
				if (!r) {
					if (p->d_type == DT_DIR)
						r = rmdir(new_path.c_str());
					else
						LOGINFO("Unable to removeDir '%s': %s\n", new_path.c_str(), strerror(errno));
				}
			} else if (p->d_type == DT_REG || p->d_type == DT_LNK || p->d_type == DT_FIFO || p->d_type == DT_SOCK) {
				r = unlink(new_path.c_str());
				if (r != 0) {
					LOGINFO("Unable to unlink '%s: %s'\n", new_path.c_str(), strerror(errno));
				}
			}
		}
		closedir(d);

		if (!r) {
			if (skipParent)
				return 0;
			else
				r = rmdir(path.c_str());
		}
	}
	return r;
}

int TWFunc::copy_file(string src, string dst, int mode) {
	LOGINFO("Copying file %s to %s\n", src.c_str(), dst.c_str());
	ifstream srcfile(src.c_str(), ios::binary);
	ofstream dstfile(dst.c_str(), ios::binary);
	dstfile << srcfile.rdbuf();
	srcfile.close();
	dstfile.close();
	if (chmod(dst.c_str(), mode) != 0)
		return -1;
	return 0;
}

unsigned int TWFunc::Get_D_Type_From_Stat(string Path) {
	struct stat st;

	stat(Path.c_str(), &st);
	if (st.st_mode & S_IFDIR)
		return DT_DIR;
	else if (st.st_mode & S_IFBLK)
		return DT_BLK;
	else if (st.st_mode & S_IFCHR)
		return DT_CHR;
	else if (st.st_mode & S_IFIFO)
		return DT_FIFO;
	else if (st.st_mode & S_IFLNK)
		return DT_LNK;
	else if (st.st_mode & S_IFREG)
		return DT_REG;
	else if (st.st_mode & S_IFSOCK)
		return DT_SOCK;
	return DT_UNKNOWN;
}

int TWFunc::read_file(string fn, string& results) {
	ifstream file;
	file.open(fn.c_str(), ios::in);

	if (file.is_open()) {
		file >> results;
		file.close();
		return 0;
	}

	LOGINFO("Cannot find file %s\n", fn.c_str());
	return -1;
}

int TWFunc::read_file(string fn, vector<string>& results) {
	ifstream file;
	string line;
	file.open(fn.c_str(), ios::in);
	if (file.is_open()) {
		while (getline(file, line))
			results.push_back(line);
		file.close();
		return 0;
	}
	LOGINFO("Cannot find file %s\n", fn.c_str());
	return -1;
}

int TWFunc::write_file(string fn, const string& line) {
	FILE *file;
	file = fopen(fn.c_str(), "w");
	if (file != NULL) {
		fwrite(line.c_str(), line.size(), 1, file);
		fclose(file);
		return 0;
	}
	LOGINFO("Cannot find file %s\n", fn.c_str());
	return -1;
}

timespec TWFunc::timespec_diff(timespec& start, timespec& end)
{
	timespec temp;
	if ((end.tv_nsec-start.tv_nsec)<0) {
		temp.tv_sec = end.tv_sec-start.tv_sec-1;
		temp.tv_nsec = 1000000000+end.tv_nsec-start.tv_nsec;
	} else {
		temp.tv_sec = end.tv_sec-start.tv_sec;
		temp.tv_nsec = end.tv_nsec-start.tv_nsec;
	}
	return temp;
}

int32_t TWFunc::timespec_diff_ms(timespec& start, timespec& end)
{
	return ((end.tv_sec * 1000) + end.tv_nsec/1000000) -
			((start.tv_sec * 1000) + start.tv_nsec/1000000);
}

int TWFunc::drop_caches(void) {
	string file = "/proc/sys/vm/drop_caches";
	string value = "3";
	if (write_file(file, value) != 0)
		return -1;
	return 0;
}

int TWFunc::Check_su_Perms(void) {
	struct stat st;
	int ret = 0;

	if (!PartitionManager.Mount_By_Path("/system", false))
		return 0;

	// Check to ensure that perms are 6755 for all 3 file locations
	if (stat("/system/bin/su", &st) == 0) {
		if ((st.st_mode & (S_ISUID | S_ISGID | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)) != (S_ISUID | S_ISGID | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) || st.st_uid != 0 || st.st_gid != 0) {
			ret = 1;
		}
	}
	if (stat("/system/xbin/su", &st) == 0) {
		if ((st.st_mode & (S_ISUID | S_ISGID | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)) != (S_ISUID | S_ISGID | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) || st.st_uid != 0 || st.st_gid != 0) {
			ret += 2;
		}
	}
	if (stat("/system/bin/.ext/.su", &st) == 0) {
		if ((st.st_mode & (S_ISUID | S_ISGID | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)) != (S_ISUID | S_ISGID | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) || st.st_uid != 0 || st.st_gid != 0) {
			ret += 4;
		}
	}
	return ret;
}

bool TWFunc::Fix_su_Perms(void) {
	if (!PartitionManager.Mount_By_Path("/system", true))
		return false;

	string propvalue = System_Property_Get("ro.build.version.sdk");
	string su_perms = "6755";
	if (!propvalue.empty()) {
		int sdk_version = atoi(propvalue.c_str());
		if (sdk_version >= 18)
			su_perms = "0755";
	}

	string file = "/system/bin/su";
	if (TWFunc::Path_Exists(file)) {
		if (chown(file.c_str(), 0, 0) != 0) {
			LOGERR("Failed to chown '%s'\n", file.c_str());
			return false;
		}
		if (tw_chmod(file, su_perms) != 0) {
			LOGERR("Failed to chmod '%s'\n", file.c_str());
			return false;
		}
	}
	file = "/system/xbin/su";
	if (TWFunc::Path_Exists(file)) {
		if (chown(file.c_str(), 0, 0) != 0) {
			LOGERR("Failed to chown '%s'\n", file.c_str());
			return false;
		}
		if (tw_chmod(file, su_perms) != 0) {
			LOGERR("Failed to chmod '%s'\n", file.c_str());
			return false;
		}
	}
	file = "/system/xbin/daemonsu";
	if (TWFunc::Path_Exists(file)) {
		if (chown(file.c_str(), 0, 0) != 0) {
			LOGERR("Failed to chown '%s'\n", file.c_str());
			return false;
		}
		if (tw_chmod(file, "0755") != 0) {
			LOGERR("Failed to chmod '%s'\n", file.c_str());
			return false;
		}
	}
	file = "/system/bin/.ext/.su";
	if (TWFunc::Path_Exists(file)) {
		if (chown(file.c_str(), 0, 0) != 0) {
			LOGERR("Failed to chown '%s'\n", file.c_str());
			return false;
		}
		if (tw_chmod(file, su_perms) != 0) {
			LOGERR("Failed to chmod '%s'\n", file.c_str());
			return false;
		}
	}
	file = "/system/etc/install-recovery.sh";
	if (TWFunc::Path_Exists(file)) {
		if (chown(file.c_str(), 0, 0) != 0) {
			LOGERR("Failed to chown '%s'\n", file.c_str());
			return false;
		}
		if (tw_chmod(file, "0755") != 0) {
			LOGERR("Failed to chmod '%s'\n", file.c_str());
			return false;
		}
	}
	file = "/system/etc/init.d/99SuperSUDaemon";
	if (TWFunc::Path_Exists(file)) {
		if (chown(file.c_str(), 0, 0) != 0) {
			LOGERR("Failed to chown '%s'\n", file.c_str());
			return false;
		}
		if (tw_chmod(file, "0755") != 0) {
			LOGERR("Failed to chmod '%s'\n", file.c_str());
			return false;
		}
	}
	file = "/system/app/Superuser.apk";
	if (TWFunc::Path_Exists(file)) {
		if (chown(file.c_str(), 0, 0) != 0) {
			LOGERR("Failed to chown '%s'\n", file.c_str());
			return false;
		}
		if (tw_chmod(file, "0644") != 0) {
			LOGERR("Failed to chmod '%s'\n", file.c_str());
			return false;
		}
	}
	sync();
	if (!PartitionManager.UnMount_By_Path("/system", true))
		return false;
	return true;
}

int TWFunc::tw_chmod(const string& fn, const string& mode) {
	long mask = 0;
	std::string::size_type n = mode.length();
	int cls = 0;

	if(n == 3)
		++cls;
	else if(n != 4)
	{
		LOGERR("TWFunc::tw_chmod used with %u long mode string (should be 3 or 4)!\n", mode.length());
		return -1;
	}

	for (n = 0; n < mode.length(); ++n, ++cls) {
		if (cls == 0) {
			if (mode[n] == '0')
				continue;
			else if (mode[n] == '1')
				mask |= S_ISVTX;
			else if (mode[n] == '2')
				mask |= S_ISGID;
			else if (mode[n] == '4')
				mask |= S_ISUID;
			else if (mode[n] == '5') {
				mask |= S_ISVTX;
				mask |= S_ISUID;
			}
			else if (mode[n] == '6') {
				mask |= S_ISGID;
				mask |= S_ISUID;
			}
			else if (mode[n] == '7') {
				mask |= S_ISVTX;
				mask |= S_ISGID;
				mask |= S_ISUID;
			}
		}
		else if (cls == 1) {
			if (mode[n] == '7') {
				mask |= S_IRWXU;
			}
			else if (mode[n] == '6') {
				mask |= S_IRUSR;
				mask |= S_IWUSR;
			}
			else if (mode[n] == '5') {
				mask |= S_IRUSR;
				mask |= S_IXUSR;
			}
			else if (mode[n] == '4')
				mask |= S_IRUSR;
			else if (mode[n] == '3') {
				mask |= S_IWUSR;
				mask |= S_IRUSR;
			}
			else if (mode[n] == '2')
				mask |= S_IWUSR;
			else if (mode[n] == '1')
				mask |= S_IXUSR;
		}
		else if (cls == 2) {
			if (mode[n] == '7') {
				mask |= S_IRWXG;
			}
			else if (mode[n] == '6') {
				mask |= S_IRGRP;
				mask |= S_IWGRP;
			}
			else if (mode[n] == '5') {
				mask |= S_IRGRP;
				mask |= S_IXGRP;
			}
			else if (mode[n] == '4')
				mask |= S_IRGRP;
			else if (mode[n] == '3') {
				mask |= S_IWGRP;
				mask |= S_IXGRP;
			}
			else if (mode[n] == '2')
				mask |= S_IWGRP;
			else if (mode[n] == '1')
				mask |= S_IXGRP;
		}
		else if (cls == 3) {
			if (mode[n] == '7') {
				mask |= S_IRWXO;
			}
			else if (mode[n] == '6') {
				mask |= S_IROTH;
				mask |= S_IWOTH;
			}
			else if (mode[n] == '5') {
				mask |= S_IROTH;
				mask |= S_IXOTH;
			}
			else if (mode[n] == '4')
				mask |= S_IROTH;
			else if (mode[n] == '3') {
				mask |= S_IWOTH;
				mask |= S_IXOTH;
			}
			else if (mode[n] == '2')
				mask |= S_IWOTH;
			else if (mode[n] == '1')
				mask |= S_IXOTH;
		}
	}

	if (chmod(fn.c_str(), mask) != 0) {
		LOGERR("Unable to chmod '%s' %l\n", fn.c_str(), mask);
		return -1;
	}

	return 0;
}

bool TWFunc::Install_SuperSU(void) {
	if (!PartitionManager.Mount_By_Path("/system", true))
		return false;

	TWFunc::Exec_Cmd("/sbin/chattr -i /system/xbin/su");
	if (copy_file("/supersu/su", "/system/xbin/su", 0755) != 0) {
		LOGERR("Failed to copy su binary to /system/bin\n");
		return false;
	}
	if (!Path_Exists("/system/bin/.ext")) {
		mkdir("/system/bin/.ext", 0777);
	}
	TWFunc::Exec_Cmd("/sbin/chattr -i /system/bin/.ext/su");
	if (copy_file("/supersu/su", "/system/bin/.ext/su", 0755) != 0) {
		LOGERR("Failed to copy su binary to /system/bin/.ext/su\n");
		return false;
	}
	TWFunc::Exec_Cmd("/sbin/chattr -i /system/xbin/daemonsu");
	if (copy_file("/supersu/su", "/system/xbin/daemonsu", 0755) != 0) {
		LOGERR("Failed to copy su binary to /system/xbin/daemonsu\n");
		return false;
	}
	if (Path_Exists("/system/etc/init.d")) {
		TWFunc::Exec_Cmd("/sbin/chattr -i /system/etc/init.d/99SuperSUDaemon");
		if (copy_file("/supersu/99SuperSUDaemon", "/system/etc/init.d/99SuperSUDaemon", 0755) != 0) {
			LOGERR("Failed to copy 99SuperSUDaemon to /system/etc/init.d/99SuperSUDaemon\n");
			return false;
		}
	} else {
		TWFunc::Exec_Cmd("/sbin/chattr -i /system/etc/install-recovery.sh");
		if (copy_file("/supersu/install-recovery.sh", "/system/etc/install-recovery.sh", 0755) != 0) {
			LOGERR("Failed to copy install-recovery.sh to /system/etc/install-recovery.sh\n");
			return false;
		}
	}
	if (copy_file("/supersu/Superuser.apk", "/system/app/Superuser.apk", 0644) != 0) {
		LOGERR("Failed to copy Superuser app to /system/app\n");
		return false;
	}
	if (!Fix_su_Perms())
		return false;
	return true;
}

bool TWFunc::Try_Decrypting_Backup(string Restore_Path, string Password) {
	DIR* d;

	string Filename;
	Restore_Path += "/";
	d = opendir(Restore_Path.c_str());
	if (d == NULL) {
		LOGERR("Error opening '%s'\n", Restore_Path.c_str());
		return false;
	}

	struct dirent* de;
	while ((de = readdir(d)) != NULL) {
		Filename = Restore_Path;
		Filename += de->d_name;
		if (TWFunc::Get_File_Type(Filename) == 2) {
			if (TWFunc::Try_Decrypting_File(Filename, Password) < 2) {
				DataManager::SetValue("tw_restore_password", ""); // Clear the bad password
				DataManager::SetValue("tw_restore_display", "");  // Also clear the display mask
				closedir(d);
				return false;
			}
		}
	}
	closedir(d);
	return true;
}

string TWFunc::Get_Current_Date() {
	string Current_Date;
	time_t seconds = time(0);
	struct tm *t = localtime(&seconds);
	char timestamp[255];
	sprintf(timestamp,"%04d-%02d-%02d--%02d-%02d-%02d",t->tm_year+1900,t->tm_mon+1,t->tm_mday,t->tm_hour,t->tm_min,t->tm_sec);
	Current_Date = timestamp;
	return Current_Date;
}

string TWFunc::System_Property_Get(string Prop_Name) {
	bool mount_state = PartitionManager.Is_Mounted_By_Path("/system");
	std::vector<string> buildprop;
	string propvalue;
	if (!PartitionManager.Mount_By_Path("/system", true))
		return propvalue;
	if (TWFunc::read_file("/system/build.prop", buildprop) != 0) {
		LOGINFO("Unable to open /system/build.prop for getting '%s'.\n", Prop_Name.c_str());
		DataManager::SetValue(TW_BACKUP_NAME, Get_Current_Date());
		if (!mount_state)
			PartitionManager.UnMount_By_Path("/system", false);
		return propvalue;
	}
	int line_count = buildprop.size();
	int index;
	size_t start_pos = 0, end_pos;
	string propname;
	for (index = 0; index < line_count; index++) {
		end_pos = buildprop.at(index).find("=", start_pos);
		propname = buildprop.at(index).substr(start_pos, end_pos);
		if (propname == Prop_Name) {
			propvalue = buildprop.at(index).substr(end_pos + 1, buildprop.at(index).size());
			if (!mount_state)
				PartitionManager.UnMount_By_Path("/system", false);
			return propvalue;
		}
	}
	if (!mount_state)
		PartitionManager.UnMount_By_Path("/system", false);
	return propvalue;
}

void TWFunc::Auto_Generate_Backup_Name() {
	string propvalue = System_Property_Get("ro.build.display.id");
	if (propvalue.empty()) {
		DataManager::SetValue(TW_BACKUP_NAME, Get_Current_Date());
		return;
	}
	string Backup_Name = Get_Current_Date();
	Backup_Name += " " + propvalue;
	if (Backup_Name.size() > MAX_BACKUP_NAME_LEN)
		Backup_Name.resize(MAX_BACKUP_NAME_LEN);
	// Trailing spaces cause problems on some file systems, so remove them
	string space_check, space = " ";
	space_check = Backup_Name.substr(Backup_Name.size() - 1, 1);
	while (space_check == space) {
		Backup_Name.resize(Backup_Name.size() - 1);
		space_check = Backup_Name.substr(Backup_Name.size() - 1, 1);
	}
	DataManager::SetValue(TW_BACKUP_NAME, Backup_Name);
	if (PartitionManager.Check_Backup_Name(false) != 0) {
		LOGINFO("Auto generated backup name '%s' contains invalid characters, using date instead.\n", Backup_Name.c_str());
		DataManager::SetValue(TW_BACKUP_NAME, Get_Current_Date());
	}
}

void TWFunc::Fixup_Time_On_Boot()
{
#ifdef QCOM_RTC_FIX
	// Devices with Qualcomm Snapdragon 800 do some shenanigans with RTC.
	// They never set it, it just ticks forward from 1970-01-01 00:00,
	// and then they have files /data/system/time/ats_* with 64bit offset
	// in miliseconds which, when added to the RTC, gives the correct time.
	// So, the time is: (offset_from_ats + value_from_RTC)
	// There are multiple ats files, they are for different systems? Bases?
	// Like, ats_1 is for modem and ats_2 is for TOD (time of day?).
	// Look at file time_genoff.h in CodeAurora, qcom-opensource/time-services

	static const char *paths[] = { "/data/system/time/", "/data/time/"  };

	DIR *d;
	FILE *f;
	uint64_t offset = 0;
	struct timeval tv;
	struct dirent *dt;
	std::string ats_path;


	// Don't fix the time of it already is over year 2000, it is likely already okay, either
	// because the RTC is fine or because the recovery already set it and then crashed
	gettimeofday(&tv, NULL);
	if(tv.tv_sec > 946684800) // timestamp of 2000-01-01 00:00:00
	{
		LOGINFO("TWFunc::Fixup_Time: not fixing time, it seems to be already okay (after year 2000).\n");
		return;
	}

	if(!PartitionManager.Mount_By_Path("/data", false))
		return;

	// Prefer ats_2, it seems to be the one we want according to logcat on hammerhead
	// - it is the one for ATS_TOD (time of day?).
	// However, I never saw a device where the offset differs between ats files.
	for(size_t i = 0; i < (sizeof(paths)/sizeof(paths[0])); ++i)
	{
		DIR *d = opendir(paths[i]);
		if(!d)
			continue;

		while((dt = readdir(d)))
		{
			if(dt->d_type != DT_REG || strncmp(dt->d_name, "ats_", 4) != 0)
				continue;

			if(ats_path.empty() || strcmp(dt->d_name, "ats_2") == 0)
				ats_path = std::string(paths[i]).append(dt->d_name);
		}

		closedir(d);
	}

	if(ats_path.empty())
	{
		LOGINFO("TWFunc::Fixup_Time: no ats files found, leaving time as-is!\n");
		return;
	}

	f = fopen(ats_path.c_str(), "r");
	if(!f)
	{
		LOGINFO("TWFunc::Fixup_Time: failed to open file %s\n", ats_path.c_str());
		return;
	}

	if(fread(&offset, sizeof(offset), 1, f) != 1)
	{
		LOGINFO("TWFunc::Fixup_Time: failed load uint64 from file %s\n", ats_path.c_str());
		fclose(f);
		return;
	}
	fclose(f);

	LOGINFO("TWFunc::Fixup_Time: Setting time offset from file %s, offset %llu\n", ats_path.c_str(), offset);

	gettimeofday(&tv, NULL);

	tv.tv_sec += offset/1000;
	tv.tv_usec += (offset%1000)*1000;

	while(tv.tv_usec >= 1000000)
	{
		++tv.tv_sec;
		tv.tv_usec -= 1000000;
	}

	settimeofday(&tv, NULL);
#endif
}

std::vector<std::string> TWFunc::Split_String(const std::string& str, const std::string& delimiter, bool removeEmpty)
{
	std::vector<std::string> res;
	size_t idx = 0, idx_last = 0;

	while(idx < str.size())
	{
		idx = str.find_first_of(delimiter, idx_last);
		if(idx == std::string::npos)
			idx = str.size();

		if(idx-idx_last != 0 || !removeEmpty)
			res.push_back(str.substr(idx_last, idx-idx_last));

		idx_last = idx + delimiter.size();
	}

	return res;
}

bool TWFunc::Create_Dir_Recursive(const std::string& path, mode_t mode, uid_t uid, gid_t gid)
{
	std::vector<std::string> parts = Split_String(path, "/");
	std::string cur_path;
	struct stat info;
	for(size_t i = 0; i < parts.size(); ++i)
	{
		cur_path += "/" + parts[i];
		if(stat(cur_path.c_str(), &info) < 0 || !S_ISDIR(info.st_mode))
		{
			if(mkdir(cur_path.c_str(), mode) < 0)
				return false;
			chown(cur_path.c_str(), uid, gid);
		}
	}
	return true;
}

bool TWFunc::loadTheme()
{
#ifndef TW_HAS_LANDSCAPE
	DataManager::SetValue(TW_ENABLE_ROTATION, 0);
#endif

	std::string base_xml = getDefaultThemePath(gr_get_rotation()) + "ui.xml";

	if (DataManager::GetIntValue(TW_IS_ENCRYPTED))
	{
		if(PageManager::LoadPackage ("TWRP", base_xml, "decrypt") != 0)
		{
			LOGERR("Failed to load base packages.\n");
			return false;
		}
		else
		{
#ifdef TW_HAS_LANDSCAPE
			DataManager::SetValue(TW_ENABLE_ROTATION, 1);
#endif
			return true;
		}
	}

	// This is for kindle fire apparently.
	// It is used to flash fire fire fire bootloader
	if (PageManager::LoadPackage("TWRP", "/script/ui.xml", "main") == 0)
	{
		DataManager::SetValue(TW_ENABLE_ROTATION, 0);
		return true;
	}

	std::string theme_path = DataManager::GetSettingsStoragePath();
	if (!PartitionManager.Mount_Settings_Storage(false))
	{
		int retry_count = 5;
		while (retry_count > 0 && !PartitionManager.Mount_Settings_Storage(false))
		{
			usleep (500000);
			retry_count--;
		}
	}

#ifndef TW_OEM_BUILD
	if (PartitionManager.Mount_Settings_Storage(false))
	{
		theme_path += getZIPThemePath(gr_get_rotation());
		if(PageManager::LoadPackage("TWRP", theme_path, "main") == 0)
		{
#ifdef TW_HAS_LANDSCAPE
			DataManager::SetValue(TW_ENABLE_ROTATION, 1);
#endif
			return true;
		}
	}
	else
		LOGERR("Unable to mount %s during GUI startup.\n", theme_path.c_str());
#endif

	if(PageManager::LoadPackage("TWRP", base_xml, "main") != 0)
	{
		LOGERR("Failed to load base packages.\n");
		return false;
	}
	else
	{
#ifdef TW_HAS_LANDSCAPE
		DataManager::SetValue(TW_ENABLE_ROTATION, 1);
#endif
		return true;
	}
}

bool TWFunc::reloadTheme()
{
	std::string theme_path = DataManager::GetSettingsStoragePath();
	if (PartitionManager.Mount_By_Path(theme_path.c_str(), 1))
	{
		theme_path += getZIPThemePath(gr_get_rotation());
		if(PageManager::ReloadPackage("TWRP", theme_path) == 0)
		{
#ifdef TW_HAS_LANDSCAPE
			DataManager::SetValue(TW_ENABLE_ROTATION, 1);
#endif
			return true;
		}
	}
	else
		LOGERR("Unable to mount %s during reload function startup.\n", theme_path.c_str());

	// Loading the custom theme failed - try loading the stock theme
	LOGINFO("Attempting to reload stock theme...\n");
	theme_path = getDefaultThemePath(gr_get_rotation()) + "ui.xml";
	if (PageManager::ReloadPackage("TWRP", theme_path) != 0)
	{
		LOGERR("Failed to load base packages.\n");
		return false;
	}
	return true;
}

std::string TWFunc::getDefaultThemePath(int rotation)
{
#ifndef TW_HAS_LANDSCAPE
	return "/res/";
#else
	if(rotation%180 == 0)
		return "/res/";
	else
		return "/res/landscape/";
#endif
}

std::string TWFunc::getZIPThemePath(int rotation)
{
#ifndef TW_HAS_LANDSCAPE
	return "/TWRP/theme/ui.zip";
#else
	if(rotation%180 == 0)
		return "/TWRP/theme/ui.zip";
	else
		return "/TWRP/theme/ui_landscape.zip";
#endif
}

std::string TWFunc::getROMName()
{
	std::string res;
	bool mount_state = PartitionManager.Is_Mounted_By_Path("/system");
	std::vector<string> buildprop;
	if (!PartitionManager.Mount_By_Path("/system", true)) {
		return res;
	}
	if (TWFunc::read_file("/system/build.prop", buildprop) != 0) {
		LOGINFO("Unable to open /system/build.prop for getting backup name.\n");
		if (!mount_state)
			PartitionManager.UnMount_By_Path("/system", false);
		return res;
	}
	int line_count = buildprop.size();
	int index;
	size_t start_pos = 0, end_pos;
	string propname;
	for (index = 0; index < line_count; index++) {
		end_pos = buildprop.at(index).find("=", start_pos);
		propname = buildprop.at(index).substr(start_pos, end_pos);
		if (propname == "ro.build.display.id") {
			res = buildprop.at(index).substr(end_pos + 1, buildprop.at(index).size());
			if (res.size() > MAX_BACKUP_NAME_LEN)
				res.resize(MAX_BACKUP_NAME_LEN);
			break;
		}
	}
	if (res.empty()) {
		LOGINFO("ro.build.display.id not found in build.prop\n");
	}
	if (!mount_state)
		PartitionManager.UnMount_By_Path("/system", false);
	return res;
}

void TWFunc::stringReplace(std::string& str, char before, char after)
{
	const size_t size = str.size();
	for(size_t i = 0; i < size; ++i)
	{
		char& c = str[i];
		if(c == before)
			c = after;
	}
}

#ifdef HAVE_SELINUX
bool TWFunc::restorecon(const std::string& path, struct selabel_handle *sh)
{
	struct stat info;
	char *oldcontext, *newcontext;
	if (lgetfilecon(path.c_str(), &oldcontext) < 0 || stat(path.c_str(), &info) < 0) {
		LOGINFO("Couldn't get selinux context and stat() for %s\n", path.c_str());
		return false;
	}
	if (selabel_lookup(sh, &newcontext, path.c_str(), info.st_mode) < 0) {
		LOGINFO("Couldn't lookup selinux context for %s\n", path.c_str());
		return false;
	}
	if (strcmp(oldcontext, newcontext) != 0) {
		LOGINFO("Relabeling %s from %s to %s\n", path.c_str(), oldcontext, newcontext);
		if (lsetfilecon(path.c_str(), newcontext) < 0) {
			LOGINFO("Couldn't label %s with %s: %s\n", path.c_str(), newcontext, strerror(errno));
		}
	}
	freecon(oldcontext);
	freecon(newcontext);
	return true;
}
#endif

#endif // ndef BUILD_TWRPTAR_MAIN
