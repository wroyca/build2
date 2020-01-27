// file      : libbuild2/config/utility.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/config/utility.hxx>

#include <libbuild2/file.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/config/module.hxx>

using namespace std;

namespace build2
{
  namespace config
  {
    pair<lookup, bool>
    omitted (scope& rs, const variable& var)
    {
      // This is a stripped-down version of the required() twisted
      // implementation.

      pair<lookup, size_t> org (rs.find_original (var));

      bool n (false); // New flag.
      lookup l (org.first);

      // Treat an inherited value that was set to default as new.
      //
      if (l.defined () && l->extra)
        n = true;

      if (var.overrides != nullptr)
      {
        pair<lookup, size_t> ovr (rs.find_override (var, move (org)));

        if (l != ovr.first) // Overriden?
        {
          // Override is always treated as new.
          //
          n = true;
          l = move (ovr.first);
        }
      }

      if (l.defined ())
        save_variable (rs, var);

      return pair<lookup, bool> (l, n);
    }

    lookup
    optional (scope& rs, const variable& var)
    {
      save_variable (rs, var);

      auto l (rs[var]);
      return l.defined ()
        ? l
        : lookup (rs.assign (var), var, rs); // NULL.
    }

    bool
    specified (scope& rs, const string& n)
    {
      // Search all outer scopes for any value in this namespace.
      //
      // What about "pure" overrides, i.e., those without any original values?
      // Well, they will also be found since their names have the original
      // variable as a prefix. But do they apply? Yes, since we haven't found
      // any original values, they will be "visible"; see find_override() for
      // details.
      //
      const variable& vns (rs.ctx.var_pool.rw (rs).insert ("config." + n));
      for (scope* s (&rs); s != nullptr; s = s->parent_scope ())
      {
        for (auto p (s->vars.find_namespace (vns));
             p.first != p.second;
             ++p.first)
        {
          const variable& var (p.first->first);

          // Ignore config.*.configured.
          //
          if (var.name.size () < 11 ||
              var.name.compare (var.name.size () - 11, 11, ".configured") != 0)
            return true;
        }
      }

      return false;
    }

    bool
    unconfigured (scope& rs, const string& n)
    {
      // Pattern-typed in boot() as bool.
      //
      const variable& var (
        rs.ctx.var_pool.rw (rs).insert ("config." + n + ".configured"));

      save_variable (rs, var);

      auto l (rs[var]); // Include inherited values.
      return l && !cast<bool> (l);
    }

    bool
    unconfigured (scope& rs, const string& n, bool v)
    {
      // Pattern-typed in boot() as bool.
      //
      const variable& var (
        rs.ctx.var_pool.rw (rs).insert ("config." + n + ".configured"));

      save_variable (rs, var);

      value& x (rs.assign (var));

      if (x.null || cast<bool> (x) != !v)
      {
        x = !v;
        return true;
      }
      else
        return false;
    }

    void
    save_variable (scope& rs, const variable& var, uint64_t flags)
    {
      if (module* m = rs.find_module<module> (module::name))
        m->save_variable (var, flags);
    }

    void
    save_module (scope& rs, const char* name, int prio)
    {
      if (module* m = rs.find_module<module> (module::name))
        m->save_module (name, prio);
    }

    void
    create_project (const dir_path& d,
                    const build2::optional<dir_path>& amal,
                    const strings& bmod,
                    const string&  rpre,
                    const strings& rmod,
                    const string&  rpos,
                    bool config,
                    bool buildfile,
                    const char* who,
                    uint16_t verbosity)
    {
      string hdr ("# Generated by " + string (who) + ". Edit if you know"
                  " what you are doing.\n"
                  "#");

      // If the directory exists, verify it's empty. Otherwise, create it.
      //
      if (exists (d))
      {
        if (!empty (d))
          fail << "directory " << d << " exists and is not empty";
      }
      else
        mkdir_p (d, verbosity);

      // Create the build/ subdirectory.
      //
      // Note that for now we use the standard build file/directory scheme.
      //
      mkdir (d / std_build_dir, verbosity);

      // Write build/bootstrap.build.
      //
      {
        path f (d / std_bootstrap_file);

        if (verb >= verbosity)
          text << (verb >= 2 ? "cat >" : "save ") << f;

        try
        {
          ofdstream ofs (f);

          ofs << hdr << endl
              << "project =" << endl;

          if (amal)
          {
            ofs << "amalgamation =";

            if (!amal->empty ())
            {
              ofs << ' ';
              to_stream (ofs, *amal, true /* representation */);
            }

            ofs << endl;
          }

          ofs << endl;

          if (config)
            ofs << "using config" << endl;

          for (const string& m: bmod)
          {
            if (!config || m != "config")
              ofs << "using " << m << endl;
          }

          ofs.close ();
        }
        catch (const io_error& e)
        {
          fail << "unable to write to " << f << ": " << e;
        }
      }

      // Write build/root.build.
      //
      {
        path f (d / std_root_file);

        if (verb >= verbosity)
          text << (verb >= 2 ? "cat >" : "save ") << f;

        try
        {
          ofdstream ofs (f);

          ofs << hdr << endl;

          if (!rpre.empty ())
            ofs << rpre << endl
                << endl;

          for (const string& cm: rmod)
          {
            // If the module name start with '?', then use optional load.
            //
            bool opt (cm.front () == '?');
            string m (cm, opt ? 1 : 0);

            // Append .config unless the module name ends with '.', in which
            // case strip it.
            //
            if (m.back () == '.')
              m.pop_back ();
            else
              m += ".config";

            ofs << "using" << (opt ? "?" : "") << " " << m << endl;
          }

          if (!rpos.empty ())
            ofs << endl
                << rpre << endl;

          ofs.close ();
        }
        catch (const io_error& e)
        {
          fail << "unable to write to " << f << ": " << e;
        }
      }

      // Write root buildfile.
      //
      if (buildfile)
      {
        path f (d / std_buildfile_file);

        if (verb >= verbosity)
          text << (verb >= 2 ? "cat >" : "save ") << f;

        try
        {
          ofdstream ofs (f);

          ofs << hdr << endl
              << "./: {*/ -build/}" << endl;

          ofs.close ();
        }
        catch (const io_error& e)
        {
          fail << "unable to write to " << f << ": " << e;
        }
      }
    }
  }
}
