// This helper runs only while inspecting or applying a user-selected offline release.
package main

import (
	"crypto/sha256"
	_ "embed"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"time"

	"github.com/sigstore/sigstore-go/pkg/bundle"
	"github.com/sigstore/sigstore-go/pkg/root"
	"github.com/sigstore/sigstore-go/pkg/verify"
)

// Public Sigstore production trust material from sigstore/root-signing, targets/trusted_root.json.
// Verification is entirely offline, including the certificate's time of signing.
//
//go:embed trusted_root.json
var trust []byte

const workflow = "https://github.com/eggleader-zhang/wwmm-remote/.github/workflows/release.yml@refs/heads/main"
const appName = "WWMM Remote"

type release struct {
	Product  string
	Platform string
	Version  string
	Build    int
	SHA256   string
}

func fileDigest(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	_, err = io.Copy(h, f)
	return hex.EncodeToString(h.Sum(nil)), err
}

// Bind all relative names, types, executable bits, file contents and relative symlink targets.
// Length-prefixed names avoid collisions even for unusual bundle filenames.
func digest(path string) (string, error) {
	st, err := os.Lstat(path)
	if err != nil {
		return "", err
	}
	if st.Mode().IsRegular() {
		return fileDigest(path)
	}
	if !st.IsDir() {
		return "", errors.New("unsupported payload")
	}
	h := sha256.New()
	err = filepath.WalkDir(path, func(p string, d fs.DirEntry, e error) error {
		if e != nil || p == path {
			return e
		}
		rel, e := filepath.Rel(path, p)
		if e != nil {
			return e
		}
		rel = filepath.ToSlash(rel)
		info, e := d.Info()
		if e != nil {
			return e
		}
		var typ, value string
		switch {
		case info.Mode()&os.ModeSymlink != 0:
			typ = "L"
			value, e = os.Readlink(p)
			if e == nil && (filepath.IsAbs(value) || !filepath.IsLocal(filepath.Join(filepath.Dir(rel), value))) {
				return fmt.Errorf("invalid bundle link %s", rel)
			}
		case d.IsDir():
			typ = "D"
		case info.Mode().IsRegular():
			typ = "F"
			value, e = fileDigest(p)
		default:
			return fmt.Errorf("unsupported file %s", rel)
		}
		if e != nil {
			return e
		}
		_, e = fmt.Fprintf(h, "%d:%s%s%o:%d:%s\n", len(rel), rel, typ, info.Mode().Perm()&0111, len(value), value)
		return e
	})
	return hex.EncodeToString(h.Sum(nil)), err
}

func validate(stage string, installed int) (release, error) {
	var r release
	path := filepath.Join(stage, "release.json")
	raw, err := os.ReadFile(path)
	if err != nil {
		return r, err
	}
	tr, err := root.NewTrustedRootFromJSON(trust)
	if err != nil {
		return r, err
	}
	verifier, err := verify.NewVerifier(tr, verify.WithSignedCertificateTimestamps(1), verify.WithTransparencyLog(1), verify.WithObserverTimestamps(1))
	if err != nil {
		return r, err
	}
	signed, err := bundle.LoadJSONFromPath(filepath.Join(stage, "release.sigstore.json"))
	if err != nil {
		return r, err
	}
	id, err := verify.NewShortCertificateIdentity("https://token.actions.githubusercontent.com", "", workflow, "")
	if err != nil {
		return r, err
	}
	sum := sha256.Sum256(raw)
	if _, err = verifier.Verify(signed, verify.NewPolicy(verify.WithArtifactDigest("sha256", sum[:]), verify.WithCertificateIdentity(id))); err != nil {
		return r, fmt.Errorf("发布签名验证失败: %w", err)
	}
	if err = json.Unmarshal(raw, &r); err != nil {
		return r, err
	}
	if r.Product != "dev.wwmm.remote" || r.Platform != runtime.GOOS || !regexp.MustCompile(`^[0-9]+\.[0-9]+\.[0-9]+$`).MatchString(r.Version) {
		return r, errors.New("安装包不属于本机平台的 WWMM Remote")
	}
	if r.Build <= installed {
		return r, errors.New("已安装相同或更新的构建")
	}
	value, err := digest(filepath.Join(stage, payloadName))
	if err != nil {
		return r, err
	}
	if value != r.SHA256 {
		return r, errors.New("安装文件与已签名的发布记录不一致")
	}
	return r, nil
}

func copyFile(src, dst string) error {
	in, e := os.Open(src)
	if e != nil {
		return e
	}
	defer in.Close()
	out, e := os.OpenFile(dst, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if e != nil {
		return e
	}
	_, e = io.Copy(out, in)
	closeErr := out.Close()
	if e != nil {
		return e
	}
	return closeErr
}

func smoke(exe string, r release) error {
	cmd := exec.Command(exe, "--installation-info")
	out, err := cmd.Output()
	if err != nil {
		return fmt.Errorf("新版无法启动: %w", err)
	}
	var info release
	if json.Unmarshal(out, &info) != nil || info.Product != r.Product || info.Version != r.Version || info.Build != r.Build {
		return errors.New("安装后的程序身份或版本不匹配")
	}
	return nil
}

func run(args []string) error {
	if len(args) == 2 && args[0] == "digest" {
		d, e := digest(args[1])
		if e == nil {
			fmt.Println(d)
		}
		return e
	}
	if len(args) < 4 {
		return errors.New("invalid update arguments")
	}
	stage := args[2]
	installed, err := strconv.Atoi(args[3])
	if err != nil || installed < 0 {
		return errors.New("invalid installed build")
	}
	switch args[0] {
	case "prepare":
		if len(args) != 5 {
			return errors.New("invalid package arguments")
		}
		if err = unpack(args[1], stage); err != nil {
			return err
		}
		r, e := validate(stage, installed)
		if e != nil {
			return e
		}
		if e = checkDestination(args[4]); e != nil {
			return e
		}
		return json.NewEncoder(os.Stdout).Encode(r)
	case "apply":
		if len(args) != 6 {
			return errors.New("invalid installation arguments")
		}
		pid, e := strconv.Atoi(args[4])
		if e != nil || pid <= 0 {
			return errors.New("invalid application PID")
		}
		r, e := validate(stage, installed)
		if e != nil {
			return e
		}
		if e = checkDestination(args[1]); e != nil {
			return e
		}
		// The parent commits to quitting only after the helper has authenticated its own staging copy.
		if e = os.WriteFile(filepath.Join(stage, "ready"), []byte("ready"), 0600); e != nil {
			return e
		}
		deadline := time.Now().Add(30 * time.Second)
		for {
			if b, err := os.ReadFile(filepath.Join(stage, "commit")); err == nil && string(b) == "install" {
				break
			}
			if time.Now().After(deadline) {
				return errors.New("应用没有确认安装，旧版可以继续使用")
			}
			time.Sleep(100 * time.Millisecond)
		}
		if e = waitExit(pid, 45*time.Second); e != nil {
			return e
		}
		return install(stage, args[1], r, args[5] == "restart")
	default:
		return errors.New("unknown update action")
	}
}
func main() {
	a := os.Args[1:]
	if err := run(a); err != nil {
		fmt.Fprintln(os.Stderr, err)
		if len(a) == 6 && a[0] == "apply" {
			os.WriteFile(filepath.Join(a[2], "error.txt"), []byte(err.Error()), 0600)
			// Only the post-handoff helper may restart the previous app after failure.
			if _, e := os.Stat(filepath.Join(a[2], "commit")); e == nil && a[5] == "restart" {
				reopen(a[1])
			}
		}
		os.Exit(1)
	}
}
