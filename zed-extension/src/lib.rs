use zed_extension_api::{self as zed, LanguageServerId, Result, Worktree};

struct TakaExtension;

impl zed::Extension for TakaExtension {
    fn new() -> Self {
        Self
    }

    fn language_server_command(
        &mut self,
        _language_server_id: &LanguageServerId,
        worktree: &Worktree,
    ) -> Result<zed::Command> {
        if let Some(path) = worktree.which("taka-lsp") {
            return Ok(zed::Command {
                command: path,
                args: Vec::new(),
                env: Vec::new(),
            });
        }

        let root = worktree.root_path();
        let script = format!("{root}/tools/taka_lsp.py");
        if std::path::Path::new(&script).is_file() {
            return Ok(zed::Command {
                command: "python3".to_string(),
                args: vec![script],
                env: Vec::new(),
            });
        }

        Err("taka-lsp not found in PATH; install it or open the Taka repository".to_string())
    }
}

zed::register_extension!(TakaExtension);
