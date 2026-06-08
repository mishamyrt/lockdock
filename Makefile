VERSION = 0.4.2

PREFIX = ${HOME}/.local/bin
THIRDPARTY_DIR = thirdparty
TARGET = target/release/lockdock

.PHONY: all clean fmt lint test install publish

all: $(TARGET)

$(TARGET): Cargo.toml Cargo.lock $(shell find crates -type f)
	cargo build --release -p lockdock

clean:
	rm -rf target

fmt:
	cargo fmt
	find crates/ \
		\( -iname '*.h' -o -iname '*.c' \) \
		-not -path "*/thirdparty/*" \
		| xargs clang-format -i

lint:
	cargo clippy --workspace --all-targets -- -D warnings

test:
	cargo test --workspace --all-targets

install: $(TARGET)
	cp $(TARGET) $(PREFIX)/lockdock

publish:
	git add Makefile
	git commit -m "chore: release ${VERSION} 🔥"
	git tag "v${VERSION}"
	git-cliff -o CHANGELOG.md
	git tag -d "v${VERSION}"
	git add CHANGELOG.md
	git commit --amend --no-edit
	git tag -a "v${VERSION}" -m "release v${VERSION}"
	git push
	git push --tags
