# Builds the menu-bar helper with swiftc. No Xcode project, no signing, no
# package manifest — this is one binary in an app bundle and the whole build is
# three commands, so a Makefile is more honest than a project file that would
# need opening to be read.

APP     := Pixelbar.app
BIN     := $(APP)/Contents/MacOS/Pixelbar
SOURCES := $(wildcard Sources/Pixelbar/*.swift)

.PHONY: all clean install uninstall run

all: $(BIN)

$(BIN): $(SOURCES) Info.plist
	@mkdir -p $(APP)/Contents/MacOS
	@cp Info.plist $(APP)/Contents/Info.plist
	swiftc -O -o $@ $(SOURCES) \
		-framework AppKit -framework SwiftUI \
		-framework AVFoundation -framework CoreAudio
	@echo "built $(APP)"

run: $(BIN)
	@open $(APP)

# ~/Applications rather than /Applications: this needs no admin rights and
# belongs to one user, whose UserDefaults hold the panel address anyway.
install: $(BIN)
	@mkdir -p $(HOME)/Applications
	@rm -rf $(HOME)/Applications/$(APP)
	@cp -R $(APP) $(HOME)/Applications/
	@mkdir -p $(HOME)/Library/LaunchAgents
	@sed 's|__APP__|$(HOME)/Applications/$(APP)|' com.pixelbar.helper.plist \
		> $(HOME)/Library/LaunchAgents/com.pixelbar.helper.plist
	@launchctl unload $(HOME)/Library/LaunchAgents/com.pixelbar.helper.plist 2>/dev/null || true
	@launchctl load $(HOME)/Library/LaunchAgents/com.pixelbar.helper.plist
	@echo "installed and started; it will come back at login"

uninstall:
	@launchctl unload $(HOME)/Library/LaunchAgents/com.pixelbar.helper.plist 2>/dev/null || true
	@rm -f $(HOME)/Library/LaunchAgents/com.pixelbar.helper.plist
	@rm -rf $(HOME)/Applications/$(APP)
	@echo "removed"

clean:
	@rm -rf $(APP)
