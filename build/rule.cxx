// file      : build/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/rule>

#include <utility>      // move()
#include <system_error>

#include <butl/filesystem>

#include <build/scope>
#include <build/algorithm>
#include <build/diagnostics>
#include <build/context>

using namespace std;
using namespace butl;

namespace build
{
  operation_rule_map rules;

  // path_rule
  //
  // Note that this rule is special. It is the last, fallback rule. If
  // it doesn't match, then no other rule can possibly match and we have
  // an error. It also cannot be ambigious with any other rule. As a
  // result the below implementation bends or ignores quite a few rules
  // that normal implementations should follow. So you probably shouldn't
  // use it as a guide to implement your own, normal, rules.
  //
  void* path_rule::
  match (action a, target& t, const string&) const
  {
    // While strictly speaking we should check for the file's existence
    // for every action (because that's the condition for us matching),
    // for some actions this is clearly a waste. Say, perform_clean: we
    // are not doing anything for this action so not checking if the file
    // exists seems harmless. What about, say, configure_update? Again,
    // whether we match or not, there is nothing to be done for this
    // action. And who knows, maybe the file doesn't exist during
    // configure_update but will magically appear during perform_update.
    // So the overall guideline seems to be this: if we don't do anything
    // for the action (other than performing it on the prerequisites),
    // then we match.
    //
    switch (a)
    {
    case perform_update_id:
      {
        path_target& pt (dynamic_cast<path_target&> (t));

        // Assign the path. While normally we shouldn't do this in match(),
        // no other rule should ever be ambiguous with the fallback one.
        //
        if (pt.path ().empty ())
          pt.derive_path ();

        return pt.mtime () != timestamp_nonexistent ? &t : nullptr;
      }
    default:
      return &t;
    }
  }

  recipe path_rule::
  apply (action a, target& t, void*) const
  {
    // Update triggers the update of this target's prerequisites
    // so it would seem natural that we should also trigger their
    // cleanup. However, this possibility is rather theoretical
    // since such an update would render this target out of date
    // which in turn would lead to an error. So until we see a
    // real use-case for this functionality, we simply ignore
    // the clean operation.
    //
    if (a.operation () == clean_id)
      return noop_recipe;

    // Search and match all the prerequisites.
    //
    search_and_match (a, t);

    return a == perform_update_id
      ? &perform_update
      : t.has_prerequisites () ? default_recipe : noop_recipe;
  }

  target_state path_rule::
  perform_update (action a, target& t)
  {
    // Make sure the target is not older than any of its prerequisites.
    //
    timestamp mt (dynamic_cast<path_target&> (t).mtime ());

    for (target* pt: t.prerequisite_targets)
    {
      target_state ts (execute (a, *pt));

      // If this is an mtime-based target, then compare timestamps.
      //
      if (auto mpt = dynamic_cast<const mtime_target*> (pt))
      {
        timestamp mp (mpt->mtime ());

        if (mt < mp)
          fail << "no recipe to " << diag_do (a, t) <<
            info << "prerequisite " << *pt << " is ahead of " << t
               << " by " << (mp - mt);
      }
      else
      {
        // Otherwise we assume the prerequisite is newer if it was changed.
        //
        if (ts == target_state::changed)
          fail << "no recipe to " << diag_do (a, t) <<
            info << "prerequisite " << *pt << " is ahead of " << t
               << " because it was updated";
      }
    }

    return target_state::unchanged;
  }

  // dir_rule
  //
  void* dir_rule::
  match (action a, target& t, const string&) const
  {
    return &t;
  }

  recipe dir_rule::
  apply (action a, target& t, void*) const
  {
    // When cleaning, ignore prerequisites that are not in the same
    // or a subdirectory of ours. For default, we don't do anything
    // other than letting our prerequisites do their thing.
    //
    switch (a.operation ())
    {
    case default_id:
    case update_id: search_and_match (a, t); break;
    case clean_id: search_and_match (a, t, t.dir); break;
    default: assert (false);
    }

    return default_recipe;
  }

  // fsdir_rule
  //
  void* fsdir_rule::
  match (action a, target& t, const string&) const
  {
    return &t;
  }

  recipe fsdir_rule::
  apply (action a, target& t, void*) const
  {
    // Inject dependency on the parent directory.
    //
    inject_parent_fsdir (a, t);

    switch (a.operation ())
    {
    // For default, we don't do anything other than letting our
    // prerequisites do their thing.
    //
    case default_id:
    case update_id:
      search_and_match (a, t);
      break;
    // For clean, ignore prerequisites that are not in the same or a
    // subdirectory of ours (if t.dir is foo/bar/, then "we" are bar
    // and our directory is foo/). Just meditate on it a bit and you
    // will see the light.
    //
    case clean_id:
      search_and_match (a, t, t.dir.root () ? t.dir : t.dir.directory ());
      break;
    default:
      assert (false);
    }

    switch (a)
    {
    case perform_update_id: return &perform_update;
    case perform_clean_id: return &perform_clean;
    default: return default_recipe; // Forward to prerequisites.
    }
  }

  target_state fsdir_rule::
  perform_update (action a, target& t)
  {
    target_state ts (target_state::unchanged);

    // First update prerequisites (e.g. create parent directories)
    // then create this directory.
    //
    if (t.has_prerequisites ())
      ts = execute_prerequisites (a, t);

    const path& d (t.dir); // Everything is in t.dir.

    // Generally, it is probably correct to assume that in the majority
    // of cases the directory will already exist. If so, then we are
    // going to get better performance by first checking if it indeed
    // exists. See try_mkdir() for details.
    //
    if (!dir_exists (d))
    {
      if (verb)
        text << "mkdir " << d;
      else
        text << "mkdir " << t;

      try
      {
        try_mkdir (d);
      }
      catch (const system_error& e)
      {
        fail << "unable to create directory " << d << ": " << e.what ();
      }

      ts = target_state::changed;
    }

    return ts;
  }

  target_state fsdir_rule::
  perform_clean (action a, target& t)
  {
    // The reverse order of update: first delete this directory,
    // then clean prerequisites (e.g., delete parent directories).
    //
    rmdir_status rs (rmdir (t.dir, t));

    target_state ts (target_state::unchanged);

    if (t.has_prerequisites ())
      ts = reverse_execute_prerequisites (a, t);

    // If we couldn't remove the directory, return postponed meaning
    // that the operation could not be performed at this time.
    //
    switch (rs)
    {
    case rmdir_status::success: return target_state::changed;
    case rmdir_status::not_empty: return target_state::postponed;
    default: return ts;
    }
  }
}
