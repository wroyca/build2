// file      : libbuild2/script/script.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_SCRIPT_SCRIPT_HXX
#define LIBBUILD2_SCRIPT_SCRIPT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/token.hxx>    // replay_tokens
#include <libbuild2/variable.hxx>

namespace build2
{
  namespace script
  {
    // Pre-parse representation.
    //

    enum class line_type
    {
      var,
      cmd,
      cmd_if,
      cmd_ifn,
      cmd_elif,
      cmd_elifn,
      cmd_else,
      cmd_end
    };

    ostream&
    operator<< (ostream&, line_type);

    struct line
    {
      line_type type;
      replay_tokens tokens;

      union
      {
        const variable* var; // Pre-entered for line_type::var.
      };
    };

    // Most of the time we will have just one line (a command).
    //
    using lines = small_vector<line, 1>;

    void
    dump (ostream&, const string& ind, const lines&);

    // Parse object model.
    //

    // redirect
    //
    enum class redirect_type
    {
      none,
      pass,
      null,
      trace,
      merge,
      here_str_literal,
      here_str_regex,
      here_doc_literal,
      here_doc_regex,
      here_doc_ref,     // Reference to here_doc literal or regex.
      file,
    };

    // Pre-parsed (but not instantiated) regex lines. The idea here is that
    // we should be able to re-create their (more or less) exact text
    // representation for diagnostics but also instantiate without any
    // re-parsing.
    //
    struct regex_line
    {
      // If regex is true, then value is the regex expression. Otherwise, it
      // is a literal. Note that special characters can be present in both
      // cases. For example, //+ is a regex, while /+ is a literal, both
      // with '+' as a special character. Flags are only valid for regex.
      // Literals falls apart into textual (has no special characters) and
      // special (has just special characters instead) ones. For example
      // foo is a textual literal, while /.+ is a special one. Note that
      // literal must not have value and special both non-empty.
      //
      bool regex;

      string value;
      string flags;
      string special;

      uint64_t line;
      uint64_t column;

      // Create regex with optional special characters.
      //
      regex_line (uint64_t l, uint64_t c,
                  string v, string f, string s = string ())
          : regex (true),
            value (move (v)),
            flags (move (f)),
            special (move (s)),
            line (l),
            column (c) {}

      // Create a literal, either text or special.
      //
      regex_line (uint64_t l, uint64_t c, string v, bool s)
          : regex (false),
            value (s ? string () : move (v)),
            special (s ? move (v) : string ()),
            line (l),
            column (c) {}
    };

    struct regex_lines
    {
      char intro;   // Introducer character.
      string flags; // Global flags (here-document).

      small_vector<regex_line, 8> lines;
    };

    // Output file redirect mode.
    //
    enum class redirect_fmode
    {
      compare,
      overwrite,
      append
    };

    struct redirect
    {
      redirect_type type;

      struct file_type
      {
        using path_type = build2::path;
        path_type path;
        redirect_fmode mode; // Meaningless for input redirect.
      };

      union
      {
        int         fd;    // Merge-to descriptor.
        string      str;   // Note: with trailing newline, if requested.
        regex_lines regex; // Note: with trailing blank, if requested.
        file_type   file;
        reference_wrapper<const redirect> ref; // Note: no chains.
      };

      string modifiers;   // Redirect modifiers.
      string end;         // Here-document end marker (no regex intro/flags).
      uint64_t end_line;  // Here-document end marker location.
      uint64_t end_column;

      // Create redirect of a type other than reference.
      //
      explicit
      redirect (redirect_type = redirect_type::none);

      // Create redirect of the reference type.
      //
      redirect (redirect_type t, const redirect& r)
          : type (redirect_type::here_doc_ref), ref (r)
      {
        // There is no support (and need) for reference chains.
        //
        assert (t == redirect_type::here_doc_ref &&
                r.type != redirect_type::here_doc_ref);
      }

      // Move constuctible/assignable-only type.
      //
      redirect (redirect&&);
      redirect& operator= (redirect&&);

      ~redirect ();

      const redirect&
      effective () const noexcept
      {
        return type == redirect_type::here_doc_ref ? ref.get () : *this;
      }
    };

    // cleanup
    //
    enum class cleanup_type
    {
      always, // &foo  - cleanup, fail if does not exist.
      maybe,  // &?foo - cleanup, ignore if does not exist.
      never   // &!foo - don’t cleanup, ignore if doesn’t exist.
    };

    // File or directory to be automatically cleaned up at the end of the
    // script execution. If the path ends with a trailing slash, then it is
    // assumed to be a directory, otherwise -- a file. A directory that is
    // about to be cleaned up must be empty.
    //
    // The last component in the path may contain a wildcard that have the
    // following semantics:
    //
    // dir/*   - remove all immediate files
    // dir/*/  - remove all immediate sub-directories (must be empty)
    // dir/**  - remove all files recursively
    // dir/**/ - remove all sub-directories recursively (must be empty)
    // dir/*** - remove directory dir with all files and sub-directories
    //           recursively
    //
    struct cleanup
    {
      cleanup_type type;
      build2::path path;
    };
    using cleanups = vector<cleanup>;

    // command_exit
    //
    enum class exit_comparison {eq, ne};

    struct command_exit
    {
      // C/C++ don't apply constraints on program exit code other than it
      // being of type int.
      //
      // POSIX specifies that only the least significant 8 bits shall be
      // available from wait() and waitpid(); the full value shall be
      // available from waitid() (read more at _Exit, _exit Open Group
      // spec).
      //
      // While the Linux man page for waitid() doesn't mention any
      // deviations from the standard, the FreeBSD implementation (as of
      // version 11.0) only returns 8 bits like the other wait*() calls.
      //
      // Windows supports 32-bit exit codes.
      //
      // Note that in shells some exit values can have special meaning so
      // using them can be a source of confusion. For bash values in the
      // [126, 255] range are such a special ones (see Appendix E, "Exit
      // Codes With Special Meanings" in the Advanced Bash-Scripting Guide).
      //
      exit_comparison comparison;
      uint8_t code;
    };

    // command
    //
    struct command
    {
      path program;
      strings arguments;

      redirect in;
      redirect out;
      redirect err;

      script::cleanups cleanups;

      command_exit exit {exit_comparison::eq, 0};
    };

    enum class command_to_stream: uint16_t
    {
      header   = 0x01,
      here_doc = 0x02,              // Note: printed on a new line.
      all      = header | here_doc
    };

    void
    to_stream (ostream&, const command&, command_to_stream);

    ostream&
    operator<< (ostream&, const command&);

    // command_pipe
    //
    using command_pipe = vector<command>;

    void
    to_stream (ostream&, const command_pipe&, command_to_stream);

    ostream&
    operator<< (ostream&, const command_pipe&);

    // command_expr
    //
    enum class expr_operator {log_or, log_and};

    struct expr_term
    {
      expr_operator op;  // OR-ed to an implied false for the first term.
      command_pipe pipe;
    };

    using command_expr = vector<expr_term>;

    void
    to_stream (ostream&, const command_expr&, command_to_stream);

    ostream&
    operator<< (ostream&, const command_expr&);

    // Script execution environment.
    //
    class environment
    {
    public:
      build2::context& context;

      // A platform the script-executed programs run at.
      //
      const target_triplet& platform;

      // Used as the builtin/process CWD and to complete relative paths. Any
      // attempt to remove or move this directory (or its parent directory)
      // using the rm or mv builtins will fail the script execution. Must be
      // an absolute path.
      //
      const dir_path& work_dir;

      // If non-empty, then any attempt to remove or move a filesystem entry
      // outside this directory using an explicit cleanup or the rm/mv
      // builtins will fail the script execution, unless the --force option is
      // specified for the builtin. Must be an absolute path, unless is empty.
      //
      const dir_path& sandbox_dir;

      // Directory names for diagnostics.
      //
      const string& work_dir_name;
      const string& sandbox_dir_name;

      environment (build2::context& ctx,
                   const target_triplet& pt,
                   const dir_path& wd, const string& wn,
                   const dir_path& sd, const string& sn)
          : context (ctx),
            platform (pt),
            work_dir (wd),
            sandbox_dir (sd),
            work_dir_name (wn),
            sandbox_dir_name (sn)
      {
      }

      // Create environment without the sandbox.
      //
      environment (build2::context& ctx,
                   const target_triplet& pt,
                   const dir_path& wd, const string& wn)
          : environment (ctx, pt, wd, wn, empty_dir_path, empty_string)
      {
      }

      // Cleanup.
      //
    public:
      script::cleanups cleanups;
      paths special_cleanups;

      // Register a cleanup. If the cleanup is explicit, then override the
      // cleanup type if this path is already registered. Ignore implicit
      // registration of a path outside root directory (see below).
      //
      void
      clean (cleanup, bool implicit);

      // Register cleanup of a special file. Such files are created to
      // maintain the script running machinery and must be removed first, not
      // to interfere with the user-defined wildcard cleanups.
      //
      void
      clean_special (path);

    public:
      // Set variable value with optional (non-empty) attributes.
      //
      // Note: see also parser::lookup_variable().
      //
      virtual void
      set_variable (string&& name, names&&, const string& attrs) = 0;

    public:
      virtual
      ~environment () = default;
    };
  }
}

#include <libbuild2/script/script.ixx>

#endif // LIBBUILD2_SCRIPT_SCRIPT_HXX
