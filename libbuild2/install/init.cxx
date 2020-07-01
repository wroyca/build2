// file      : libbuild2/install/init.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/install/init.hxx>

#include <libbutl/command.mxx> // command_substitute()

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/rule.hxx>
#include <libbuild2/function.hxx>
#include <libbuild2/operation.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/config/utility.hxx>

#include <libbuild2/install/rule.hxx>
#include <libbuild2/install/utility.hxx>
#include <libbuild2/install/operation.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace install
  {
    // Process an install.<name>.* value replacing the <var>-substitutions
    // with their actual values. Note that for now we are only doing this for
    // dir_path (install.<name> variables).
    //
    // The semantics of <>-substitution is inspired by our command running
    // facility. In a nutshell, `<<` is an escape, unknown or unterminated
    // substitution is an error.
    //
    //
    template<typename T>
    static inline T
    proc_var (scope&, const T& val, const variable&)
    {
      return val;
    }

    static inline dir_path
    proc_var (scope& rs, const dir_path& val, const variable& var)
    {
      if (val.string ().find ('<') == string::npos)
        return val;

      // Note: watch out for the small std::function optimization.
      //
      auto subst = [&rs] (const string& var, string& r)
      {
        if (var == "project")
          r += project (rs).string ();
        else
          return false;

        return true;
      };

      dir_path r;
      for (auto i (val.begin ()); i != val.end (); ++i)
      {
        auto s (*i);

        try
        {
          size_t sp (s.find ('<'));
          r.combine (sp == string::npos
                     ? s
                     : command_substitute (s, sp, subst, '<', '>'),
                     i.separator ());
        }
        catch (const invalid_argument& e)
        {
          fail << "invalid " << var << " value '" << val << "': " << e;
        }
      }

      return r;
    }

    // Set an install.<name>.* value based on config.install.<name>.* or the
    // default. If none of config.install.* values were specified (spec is
    // false), then we do omitted/delayed configuration. Note that we still
    // need to set all the install.* values to defaults, as if we had the
    // default configuration.
    //
    // If override is true, then override values that came from outer
    // configurations. We had to do this for paths that contain the project
    // name but now we use the <project> substitution. Let's keep this
    // functionality for now in case we need it for something else.
    //
    // For global values we only set config.install.* variables. Non-global
    // values with NULL defaults are omitted.
    //
    template <typename T, typename CT>
    static void
    set_var (bool spec,
             scope& rs,
             const char* name,
             const char* var,
             const CT* dv,
             bool override = false)
    {
      string vn;
      lookup l;

      bool global (*name == '\0');

      if (spec)
      {
        vn = "config.install";
        if (!global)
        {
          vn += '.';
          vn += name;
        }
        vn += var;
        const variable& vr (rs.var_pool ().insert<CT> (move (vn)));

        using config::lookup_config;

        l = dv != nullptr
          ? lookup_config (rs, vr, *dv, 0 /* save_flags */, override)
          : (global
             ? lookup_config (rs, vr, nullptr)
             : lookup_config (rs, vr));
      }

      if (global)
        return;

      vn = "install.";
      vn += name;
      vn += var;
      const variable& vr (rs.var_pool ().insert<T> (move (vn)));

      value& v (rs.assign (vr));

      if (spec)
      {
        if (l)
          v = proc_var (rs, cast<T> (l), vr); // Strip CT to T.
      }
      else
      {
        if (dv != nullptr)
          v = proc_var (rs, *dv, vr);
      }
    }

    template <typename T>
    static void
    set_dir (bool s,                                  // specified
             scope& rs,                               // root scope
             const char* n,                           // var name
             const T& p,                              // path
             bool o = false,                          // override
             const string& fm = string (),            // file mode
             const string& dm = string (),            // dir mode
             const build2::path& c = build2::path ()) // command
    {
      using build2::path;

      bool global (*n == '\0');

      if (!global)
        set_var<dir_path> (s, rs, n, "",        p.empty ()  ? nullptr : &p, o);

      set_var<path>     (s, rs, n, ".cmd",      c.empty ()  ? nullptr : &c);
      set_var<strings>  (s, rs, n, ".options",  (strings*) (nullptr));
      set_var<string>   (s, rs, n, ".mode",     fm.empty () ? nullptr : &fm);
      set_var<string>   (s, rs, n, ".dir_mode", dm.empty () ? nullptr : &dm);
      set_var<string>   (s, rs, n, ".sudo",     (string*) (nullptr));

      // This one doesn't have config.* value (only set in a buildfile).
      //
      if (!global)
        rs.var_pool ().insert<bool> (string ("install.") + n + ".subdirs");
    }

    void
    functions (function_map&); // functions.cxx

    bool
    boot (scope& rs, const location&, module_boot_extra&)
    {
      tracer trace ("install::boot");
      l5 ([&]{trace << "for " << rs;});

      context& ctx (rs.ctx);

      // Register the install function family if this is the first instance of
      // the install modules.
      //
      if (!function_family::defined (ctx.functions, "install"))
        functions (ctx.functions);

      // Register our operations.
      //
      rs.insert_operation (install_id,            op_install);
      rs.insert_operation (uninstall_id,          op_uninstall);
      rs.insert_operation (update_for_install_id, op_update_for_install);

      return false;
    }

    static const path cmd ("install");

#define DIR(N, V) static const dir_path dir_##N (V)

    DIR (data_root,  dir_path ("root"));
    DIR (exec_root,  dir_path ("root"));

    DIR (sbin,       dir_path ("exec_root") /= "sbin");
    DIR (bin,        dir_path ("exec_root") /= "bin");
    DIR (lib,        dir_path ("exec_root") /= "lib");
    DIR (libexec,   (dir_path ("exec_root") /= "libexec") /= "<project>");
    DIR (pkgconfig,  dir_path ("lib")       /= "pkgconfig");

    DIR (include,    dir_path ("data_root") /= "include");
    DIR (share,      dir_path ("data_root") /= "share");
    DIR (data,       dir_path ("share")     /= "<project>");

    DIR (doc,       (dir_path ("share")     /= "doc") /= "<project>");
    DIR (legal,      dir_path ("doc"));
    DIR (man,        dir_path ("share")     /= "man");
    DIR (man1,       dir_path ("man")       /= "man1");

#undef DIR

    static const group_rule group_rule_ (true /* see_through_only */);

    bool
    init (scope& rs,
          scope& bs,
          const location& l,
          bool first,
          bool,
          module_init_extra&)
    {
      tracer trace ("install::init");

      if (!first)
      {
        warn (l) << "multiple install module initializations";
        return true;
      }

      l5 ([&]{trace << "for " << rs;});

      // Enter module variables.
      //
      auto& vp (rs.var_pool ());

      // Note that the set_dir() calls below enter some more.
      //
      {
        // The install variable is a path, not dir_path, since it can be used
        // to both specify the target directory (to install with the same file
        // name) or target file (to install with a different name). And the
        // way we distinguish between the two is via the presence/absence of
        // the trailing directory separator.
        //
        vp.insert<path>   ("install",     variable_visibility::target);
        vp.insert<bool>   ("for_install", variable_visibility::prereq);
        vp.insert<string> ("install.mode");
        vp.insert<bool>   ("install.subdirs");
      }

      // Register our rules.
      //
      {
        const auto& ar (alias_rule::instance);
        const auto& dr (fsdir_rule::instance);
        const auto& fr (file_rule::instance);
        const auto& gr (group_rule_);

        bs.insert_rule<alias> (perform_install_id,   "install.alias", ar);
        bs.insert_rule<alias> (perform_uninstall_id, "uninstall.alias", ar);

        bs.insert_rule<fsdir> (perform_install_id,   "install.fsdir", dr);
        bs.insert_rule<fsdir> (perform_uninstall_id, "install.fsdir", dr);

        bs.insert_rule<file> (perform_install_id,   "install.file", fr);
        bs.insert_rule<file> (perform_uninstall_id, "uninstall.file", fr);

        bs.insert_rule<target> (perform_install_id,   "install.file", gr);
        bs.insert_rule<target> (perform_uninstall_id, "uninstall.file", gr);
     }

      // Configuration.
      //
      // Note that we don't use any defaults for root -- the location
      // must be explicitly specified or the installer will complain
      // if and when we try to install.
      //
      {
        using build2::path;
        using config::lookup_config;
        using config::specified_config;

        bool s (specified_config (rs, "install"));

        // Adjust module priority so that the (numerous) config.install.*
        // values are saved at the end of config.build.
        //
        if (s)
          config::save_module (rs, "install", INT32_MAX);

        // Global config.install.* values.
        //
        set_dir (s, rs, "",          abs_dir_path (), false, "644", "755", cmd);

        set_dir (s, rs, "root",      abs_dir_path ());

        set_dir (s, rs, "data_root", dir_data_root);
        set_dir (s, rs, "exec_root", dir_exec_root, false, "755");

        set_dir (s, rs, "sbin",      dir_sbin);
        set_dir (s, rs, "bin",       dir_bin);
        set_dir (s, rs, "lib",       dir_lib);
        set_dir (s, rs, "libexec",   dir_libexec);
        set_dir (s, rs, "pkgconfig", dir_pkgconfig, false, "644");

        set_dir (s, rs, "include",   dir_include);
        set_dir (s, rs, "share",     dir_share);
        set_dir (s, rs, "data",      dir_data);

        set_dir (s, rs, "doc",       dir_doc);
        set_dir (s, rs, "legal",     dir_legal);
        set_dir (s, rs, "man",       dir_man);
        set_dir (s, rs, "man1",      dir_man1);

        // Support for chroot'ed install (aka DESTDIR).
        //
        {
          auto& var  (vp.insert<dir_path>     (       "install.chroot"));
          auto& cvar (vp.insert<abs_dir_path> ("config.install.chroot"));

          value& v (rs.assign (var));

          if (s)
          {
            if (lookup l = lookup_config (rs, cvar, nullptr))
              v = cast<dir_path> (l); // Strip abs_dir_path.
          }
        }
      }

      // Configure "installability" for built-in target types.
      //
      install_path<exe>   (bs, dir_path ("bin"));
      install_path<doc>   (bs, dir_path ("doc"));
      install_path<legal> (bs, dir_path ("legal"));
      install_path<man>   (bs, dir_path ("man"));
      install_path<man1>  (bs, dir_path ("man1"));

      return true;
    }

    static const module_functions mod_functions[] =
    {
      {"install", &boot,   &init},
      {nullptr,   nullptr, nullptr}
    };

    const module_functions*
    build2_install_load ()
    {
      return mod_functions;
    }
  }
}
