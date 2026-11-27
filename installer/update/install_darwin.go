package main

import (
	"errors"
	"fmt"
	"golang.org/x/sys/unix"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

const payloadName = appName + ".app"

func unpack(src, stage string) error {
	image := filepath.Join(stage, "source.dmg")
	if e := copyFile(src, image); e != nil {
		return e
	}
	defer os.Remove(image)
	mount := filepath.Join(stage, "volume")
	if e := os.Mkdir(mount, 0700); e != nil {
		return e
	}
	defer os.Remove(mount)
	if out, e := exec.Command("/usr/bin/hdiutil", "attach", "-readonly", "-nobrowse", "-noautoopen", "-mountpoint", mount, image).CombinedOutput(); e != nil {
		return fmt.Errorf("无法打开磁盘映像: %s %w", out, e)
	}
	defer func() {
		if exec.Command("/usr/bin/hdiutil", "detach", mount).Run() != nil {
			exec.Command("/usr/bin/hdiutil", "detach", "-force", mount).Run()
		}
	}()
	// Only regular metadata and a real application directory are accepted at the mount root.
	for _, n := range []string{"release.json", "release.sigstore.json"} {
		p := filepath.Join(mount, "."+n)
		i, e := os.Lstat(p)
		if e != nil || !i.Mode().IsRegular() || i.Size() > 1<<20 {
			return errors.New("磁盘映像缺少发布签名")
		}
		if e = copyFile(p, filepath.Join(stage, n)); e != nil {
			return e
		}
	}
	app := filepath.Join(mount, payloadName)
	i, e := os.Lstat(app)
	if e != nil || !i.IsDir() {
		return errors.New("缺少应用程序")
	}
	return exec.Command("/usr/bin/ditto", app, filepath.Join(stage, payloadName)).Run()
}
func checkDestination(target string) error {
	if !filepath.IsAbs(target) || !strings.HasSuffix(target, ".app") {
		return errors.New("请从已安装的应用程序启动更新")
	}
	st, err := os.Lstat(target)
	if err != nil || !st.IsDir() {
		return errors.New("无效的应用程序目录")
	}
	id, err := exec.Command("/usr/bin/plutil", "-extract", "CFBundleIdentifier", "raw", "-o", "-", filepath.Join(target, "Contents/Info.plist")).Output()
	if err != nil || strings.TrimSpace(string(id)) != "dev.wwmm.remote" {
		return errors.New("目标应用不是 WWMM Remote")
	}
	dir, e := os.MkdirTemp(filepath.Dir(target), ".wwmm-check-")
	if e != nil {
		return errors.New("此位置不可写。请从磁盘映像将应用拖入可写的「应用程序」目录")
	}
	return os.Remove(dir)
}
func waitExit(pid int, timeout time.Duration) error {
	end := time.Now().Add(timeout)
	for time.Now().Before(end) {
		if errors.Is(unix.Kill(pid, 0), unix.ESRCH) {
			return nil
		}
		time.Sleep(100 * time.Millisecond)
	}
	return errors.New("等待应用退出超时")
}
func reopen(target string) error { return exec.Command("/usr/bin/open", "-n", target).Run() }
func install(stage, target string, r release, restart bool) error {
	parent, e := os.MkdirTemp(filepath.Dir(target), ".wwmm-install-")
	if e != nil {
		return e
	}
	next := filepath.Join(parent, payloadName)
	clean := true
	defer func() {
		if clean {
			os.RemoveAll(parent)
		}
	}()
	if e = exec.Command("/usr/bin/ditto", filepath.Join(stage, payloadName), next).Run(); e != nil {
		return e
	}
	h, e := digest(next)
	if e != nil || h != r.SHA256 {
		return errors.New("应用复制校验失败")
	}
	if e = exec.Command("/usr/bin/codesign", "--verify", "--deep", "--strict", next).Run(); e != nil {
		return errors.New("应用代码签名无效")
	}
	bin := func(app string) string { return filepath.Join(app, "Contents/MacOS", appName) }
	if e = smoke(bin(next), r); e != nil {
		return e
	}
	if e = unix.RenamexNp(next, target, unix.RENAME_SWAP); e != nil {
		return e
	}
	if e = smoke(bin(target), r); e == nil && restart {
		e = reopen(target)
	}
	if e != nil {
		if rollback := unix.RenamexNp(next, target, unix.RENAME_SWAP); rollback != nil {
			clean = false
			return fmt.Errorf("恢复失败，旧版保存在 %s: %w", next, rollback)
		}
		return e
	}
	return nil
}
