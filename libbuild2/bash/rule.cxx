// file      : libbuild2/bash/rule.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/bash/rule.hxx>

#include <cstring> // strlen(), strchr()

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/in/target.hxx>

#include <libbuild2/bash/target.hxx>
#include <libbuild2/bash/utility.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace bash
  {
    using in::in;

    struct match_data
    {
      // The "for install" condition is signalled to us by install_rule when
      // it is matched for the update operation. It also verifies that if we
      // have already been executed, then it was for install.
      //
      // See cc::link_rule for a discussion of some subtleties in this logic.
      //
      optional<bool> for_install;
    };

    static_assert (sizeof (match_data) <= target::data_size,
                   "insufficient space");

    // in_rule
    //
    bool in_rule::
    match (action a, target& t, const string&) const
    {
      tracer trace ("bash::in_rule::match");

      // Note that for bash{} we match even if the target does not depend on
      // any modules (while it could have been handled by the in module, that
      // would require loading it).
      //
      bool fi (false);           // Found in.
      bool fm (t.is_a<bash> ()); // Found module.
      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        if (include (a, t, p) != include_type::normal) // Excluded/ad hoc.
          continue;

        fi = fi || p.is_a<in> ();
        fm = fm || p.is_a<bash> ();
      }

      if (!fi)
        l4 ([&]{trace << "no in file prerequisite for target " << t;});

      if (!fm)
        l4 ([&]{trace << "no bash module prerequisite for target " << t;});

      return (fi && fm);
    }

    recipe in_rule::
    apply (action a, target& t) const
    {
      // Note that for-install is signalled by install_rule and therefore
      // can only be relied upon during execute.
      //
      t.data (match_data ());

      return rule::apply (a, t);
    }

    target_state in_rule::
    perform_update (action a, const target& t) const
    {
      // Unless the outer install rule signalled that this is update for
      // install, signal back that we've performed plain update.
      //
      match_data& md (t.data<match_data> ());

      if (!md.for_install)
        md.for_install = false;

      return rule::perform_update (a, t);
    }

    prerequisite_target in_rule::
    search (action a,
            const target& t,
            const prerequisite_member& pm,
            include_type i) const
    {
      tracer trace ("bash::in_rule::search");

      // Handle import of installed bash{} modules.
      //
      if (i == include_type::normal && pm.proj () && pm.is_a<bash> ())
      {
        // We only need this during update.
        //
        if (a != perform_update_id)
          return nullptr;

        const prerequisite& p (pm.prerequisite);

        // Form the import path.
        //
        // Note that unless specified, we use the standard .bash extension
        // instead of going through the bash{} target type since this path is
        // not in our project (and thus no project-specific customization
        // apply).
        //
        string ext (p.ext ? *p.ext : "bash");
        path ip (dir_path (project_base (*p.proj)) / p.dir / p.name);

        if (!ext.empty ())
        {
          ip += '.';
          ip += ext;
        }

        // Search in PATH, similar to butl::path_search().
        //
        if (optional<string> s = getenv ("PATH"))
        {
          for (const char* b (s->c_str ()), *e;
               b != nullptr;
               b = (e != nullptr ? e + 1 : e))
          {
            e = strchr (b, path::traits_type::path_separator);

            // Empty path (i.e., a double colon or a colon at the beginning or
            // end of PATH) means search in the current dirrectory. We aren't
            // going to do that. Also silently skip invalid paths, stat()
            // errors, etc.
            //
            if (size_t n = (e != nullptr ? e - b : strlen (b)))
            {
              try
              {
                path ap (b, n);
                ap /= ip;
                ap.normalize ();

                timestamp mt (file_mtime (ap));

                if (mt != timestamp_nonexistent)
                {
                  auto rp (t.ctx.targets.insert_locked (bash::static_type,
                                                        ap.directory (),
                                                        dir_path () /* out */,
                                                        p.name,
                                                        ext,
                                                        true /* implied */,
                                                        trace));

                  bash& pt (rp.first.as<bash> ());

                  // Only set mtime/path on first insertion.
                  //
                  if (rp.second.owns_lock ())
                  {
                    pt.mtime (mt);
                    pt.path (move (ap));
                  }

                  // Save the length of the import path in auxuliary data. We
                  // use it in substitute_import() to infer the installation
                  // directory.
                  //
                  return prerequisite_target (&pt, i, ip.size ());
                }
              }
              catch (const invalid_path&) {}
              catch (const system_error&) {}
            }
          }
        }

        // Let standard search() handle it.
      }

      return rule::search (a, t, pm, i);
    }

    optional<string> in_rule::
    substitute (const location& l,
                action a,
                const target& t,
                const string& n,
                bool strict) const
    {
      return n.compare (0, 6, "import") == 0 && (n[6] == ' ' || n[6] == '\t')
        ? substitute_import (l, a, t, trim (string (n, 7)))
        : rule::substitute (l, a, t, n, strict);
    }

    string in_rule::
    substitute_import (const location& l,
                       action a,
                       const target& t,
                       const string& n) const
    {
      // Derive (relative) import path from the import name.
      //
      path ip;

      try
      {
        ip = path (n);

        if (ip.empty () || ip.absolute ())
          throw invalid_path (n);

        if (ip.extension_cstring () == nullptr)
          ip += ".bash";

        ip.normalize ();
      }
      catch (const invalid_path&)
      {
        fail (l) << "invalid import path '" << n << "'";
      }

      // Look for a matching prerequisite.
      //
      const path* ap (nullptr);
      for (const prerequisite_target& pt: t.prerequisite_targets[a])
      {
        if (pt.target == nullptr || pt.adhoc)
          continue;

        if (const bash* b = pt.target->is_a<bash> ())
        {
          const path& pp (b->path ());
          assert (!pp.empty ()); // Should have been assigned by update.

          // The simple "tail match" can be ambigous. Consider, for example,
          // the foo/bar.bash import path and /.../foo/bar.bash as well as
          // /.../x/foo/bar.bash prerequisites: they would both match.
          //
          // So the rule is the match must be from the project root directory
          // or from the installation directory for the import-installed
          // prerequisites.
          //
          // But we still do a simple match first since it can quickly weed
          // out candidates that cannot possibly match.
          //
          if (!pp.sup (ip))
            continue;

          // See if this is import-installed target (refer to search() for
          // details).
          //
          if (size_t n = pt.data)
          {
            // Both are normalized so we can compare the "tails".
            //
            const string& ps (pp.string ());
            const string& is (ip.string ());

            if (path::traits_type::compare (
                  ps.c_str () + ps.size () - n, n,
                  is.c_str (),                  is.size ()) == 0)
            {
              ap = &pp;
              break;
            }
            else
              continue;
          }

          if (const scope* rs = t.ctx.scopes.find (b->dir).root_scope ())
          {
            const dir_path& d (pp.sub (rs->src_path ())
                               ? rs->src_path ()
                               : rs->out_path ());

            if (pp.leaf (d) == ip)
            {
              ap = &pp;
              break;
            }
            else
              continue;
          }

          fail (l) << "target " << *b << " is out of project nor imported";
        }
      }

      if (ap == nullptr)
        fail (l) << "unable to resolve import path " << ip;

      match_data& md (t.data<match_data> ());
      assert (md.for_install);

      if (*md.for_install)
      {
        // For the installed case we assume the script and all its modules are
        // installed into the same location (i.e., the same bin/ directory)
        // and so we use the path relative to the script.
        //
        // BTW, the semantics of the source builtin in bash is to search in
        // PATH if it's a simple path (that is, does not contain directory
        // components) and then in the current working directory.
        //
        // So we have to determine the scripts's directory ourselves for which
        // we use the BASH_SOURCE array. Without going into the gory details,
        // the last element in this array is the script's path regardless of
        // whether we are in the script or (sourced) module (but it turned out
        // not to be what we need; see below).
        //
        // We also want to get the script's "real" directory even if it was
        // itself symlinked somewhere else. And this is where things get
        // hairy: we could use either realpath or readlink -f but neither is
        // available on Mac OS (there is readlink but it doesn't support the
        // -f option).
        //
        // One can get GNU readlink from Homebrew but it will be called
        // greadlink. Note also that for any serious development one will
        // probably be also getting newer bash from Homebrew since the system
        // one is stuck in the GPLv2 version 3.2.X era. So a bit of a mess.
        //
        // For now let's use readlink -f and see how it goes. If someone wants
        // to use/support their scripts on Mac OS, they have several options:
        //
        // 1. Install greadlink (coreutils) and symlink it as readlink.
        //
        // 2. Add the readlink function to their script that does nothing;
        //    symlinking scripts won't be supported but the rest should work
        //    fine.
        //
        // 3. Add the readlink function to their script that calls greadlink.
        //
        // 4. Add the readlink function to their script that implements the
        //    -f mode (or at least the part of it that we need). See the bash
        //    module tests for some examples.
        //
        // In the future we could automatically inject an implementation along
        // the (4) lines at the beginning of the script.
        //
        // Note also that we really, really want to keep the substitution a
        // one-liner since the import can be in an (indented) if-block, etc.,
        // and we still want the resulting scripts to be human-readable.
        //
        if (t.is_a<exe> ())
        {
          return
            "source \"$(dirname"
            " \"$(readlink -f"
            " \"${BASH_SOURCE[0]}\")\")/"
            + ip.string () + "\"";
        }
        else
        {
          // Things turned out to be trickier for the installed modules: we
          // cannot juts use the script's path since it itself might not be
          // installed (import installed). So we have to use the importer's
          // path and calculate its "offset" to the installation directory.
          //
          dir_path d (t.dir.leaf (t.root_scope ().out_path ()));

          string o;
          for (auto i (d.begin ()), e (d.end ()); i != e; ++i)
            o += "../";

          // Here we don't use readlink since we assume nobody will symlink
          // the modules (or they will all be symlinked together).
          //
          return
            "source \"$(dirname"
            " \"${BASH_SOURCE[0]}\")/"
            + o + ip.string () + "\"";
        }
      }
      else
        return "source " + ap->string ();
    }

    // install_rule
    //
    bool install_rule::
    match (action a, target& t, const string& hint) const
    {
      // We only want to handle installation if we are also the ones building
      // this target. So first run in's match().
      //
      return in_.match (a, t, hint) && file_rule::match (a, t, "");
    }

    recipe install_rule::
    apply (action a, target& t) const
    {
      recipe r (file_rule::apply (a, t));

      if (a.operation () == update_id)
      {
        // Signal to the in rule that this is update for install. And if the
        // update has already been executed, verify it was done for install.
        //
        auto& md (t.data<match_data> ());

        if (md.for_install)
        {
          if (!*md.for_install)
            fail << "target " << t << " already updated but not for install";
        }
        else
          md.for_install = true;
      }

      return r;
    }

    const target* install_rule::
    filter (action a, const target& t, const prerequisite& p) const
    {
      // If this is a module prerequisite, install it as long as it is in the
      // same amalgamation as we are.
      //
      if (p.is_a<bash> ())
      {
        const target& pt (search (t, p));
        return pt.in (t.weak_scope ()) ? &pt : nullptr;
      }
      else
        return file_rule::filter (a, t, p);
    }
  }
}
