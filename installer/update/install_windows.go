package main

import (
	"errors"
	"fmt"
	"golang.org/x/sys/windows"
	"os"
	"os/exec"
	"path/filepath"
	"time"
)

const payloadName = "install.exe"

func unpack(src, stage string) error {
	// LoadLibraryAsDataFile never executes entry points from a selected installer.
	full, e := filepath.Abs(src)
	if e != nil {
		return e
	}
	h, e := windows.LoadLibraryEx(full, 0, windows.LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE|windows.LOAD_LIBRARY_AS_IMAGE_RESOURCE)
	if e != nil {
		return e
	}
	defer windows.FreeLibrary(h)
	for id, n := range map[int]string{201: payloadName, 202: "release.json", 203: "release.sigstore.json"} {
		r, e := windows.FindResource(h, windows.ResourceID(id), windows.RT_RCDATA)
		if e != nil {
			return errors.New("安装包缺少 WWMM 发布数据")
		}
		b, e := windows.LoadResourceData(h, r)
		if e != nil {
			return e
		}
		if id != 201 && len(b) > 1<<20 {
			return errors.New("发布信息过大")
		}
		if e = os.WriteFile(filepath.Join(stage, n), b, 0600); e != nil {
			return e
		}
	}
	return nil
}
func checkDestination(dir string) error {
	if !filepath.IsAbs(dir) {
		return errors.New("无效的安装目录")
	}
	b, e := os.ReadFile(filepath.Join(dir, "wwmm-install.id"))
	if e != nil || string(b) != "dev.wwmm.remote\n" {
		return errors.New("请先使用 Windows 安装程序安装一次，再从托盘升级")
	}
	return nil
}
func waitExit(pid int, timeout time.Duration) error {
	h, e := windows.OpenProcess(windows.SYNCHRONIZE, false, uint32(pid))
	if errors.Is(e, windows.ERROR_INVALID_PARAMETER) {
		return nil
	}
	if e != nil {
		return e
	}
	defer windows.CloseHandle(h)
	result, e := windows.WaitForSingleObject(h, uint32(timeout.Milliseconds()))
	if e != nil {
		return e
	}
	if result != windows.WAIT_OBJECT_0 {
		return errors.New("等待应用退出超时")
	}
	return nil
}
func reopen(target string) error { return exec.Command(filepath.Join(target, appName+".exe")).Start() }
func install(stage, target string, r release, restart bool) error {
	backup, e := os.MkdirTemp(filepath.Dir(target), ".wwmm-backup-")
	if e != nil {
		return e
	}
	old := filepath.Join(backup, "previous")
	if e = os.Rename(target, old); e != nil {
		os.Remove(backup)
		return e
	}
	installer := exec.Command(filepath.Join(stage, payloadName), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/NOCLOSEAPPLICATIONS", "/DIR="+target)
	if e = installer.Run(); e == nil {
		e = smoke(filepath.Join(target, appName+".exe"), r)
	}
	if e == nil && restart {
		e = reopen(target)
	}
	if e != nil {
		// Move partial output aside before restoring. Leave the backup intact if restoring fails.
		if _, err := os.Stat(target); err == nil {
			if err = os.Rename(target, filepath.Join(backup, "failed")); err != nil {
				return fmt.Errorf("旧版保存在 %s: %w", old, err)
			}
		}
		if err := os.Rename(old, target); err != nil {
			return fmt.Errorf("旧版保存在 %s: %w", old, err)
		}
	}
	os.RemoveAll(backup)
	return e
}
