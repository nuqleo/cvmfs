cvmfs_server_publish() {
  local names
  local user
  local spool_dir
  local stratum0
  local upstream
  local hash_algorithm
  local tweaks_option=
  local tag_name=
  local tag_channel=00
  local tag_description=
  local retcode=0
  local verbosity=""
  local manual_revision=""
  local gc_timespan=0
  local authz_file=""
  local force_external=0
  local force_native=0
  local force_compression_algorithm=""
  local external_option=""
  local open_fd_dialog=1

  # optional parameter handling
  OPTIND=1
  while getopts "F:NXZ:pa:c:m:vn:f" option
  do
    case $option in
      p)
        tweaks_option="-d"
      ;;
      a)
        tag_name="$OPTARG"
      ;;
      c)
        tag_channel="$OPTARG"
      ;;
      m)
        tag_description="$OPTARG"
      ;;
      v)
        verbosity="-x"
      ;;
      n)
        manual_revision="$OPTARG"
      ;;
      X)
        force_external=1
      ;;
      N)
        force_native=1
      ;;
      Z)
        force_compression_algorithm="$OPTARG"
      ;;
      F)
        authz_file="-F $OPTARG"
      ;;
      f)
        open_fd_dialog=0
      ;;
      ?)
        shift $(($OPTIND-2))
        usage "Command publish: Unrecognized option: $1"
      ;;
    esac
  done

  if [ $(($force_external + $force_native)) -eq 2 ]; then
    usage "Command publish: -N and -X are mutually exclusive"
  fi

  # get repository names
  shift $(($OPTIND-1))
  check_parameter_count_for_multiple_repositories $#
  names=$(get_or_guess_multiple_repository_names "$@")
  check_multiple_repository_existence "$names"

  # sanity checks
  if [ ! -z "$tag_name" ]; then
    echo $tag_name | grep -q -v " "       || die "Spaces are not allowed in tag names"
    check_tag_existence $name "$tag_name" && die "Tag name '$tag_name' is already in use."
  fi

  for name in $names; do

    # sanity checks
    is_stratum0 $name   || die "This is not a stratum 0 repository"
    is_publishing $name && die "Another publish process is active for $name"
    health_check -r $name

    # get repository information
    load_repo_config $name
    user=$CVMFS_USER
    spool_dir=$CVMFS_SPOOL_DIR
    scratch_dir="${spool_dir}/scratch/current"
    stratum0=$CVMFS_STRATUM0
    upstream=$CVMFS_UPSTREAM_STORAGE
    hash_algorithm="${CVMFS_HASH_ALGORITHM-sha1}"
    compression_alg="${CVMFS_COMPRESSION_ALGORITHM-default}"
    if [ x"$force_compression_algorithm" != "x" ]; then
      compression_alg="$force_compression_algorithm"
    fi
    if [ x"$CVMFS_EXTERNAL_DATA" = "xtrue" -o $force_external -eq 1 ]; then
      if [ $force_native -eq 0 ]; then
        external_option="-Y"
      fi
    fi

    # more sanity checks
    is_owner_or_root $name || { echo "Permission denied: Repository $name is owned by $user"; retcode=1; continue; }
    check_repository_compatibility $name
    check_expiry $name $stratum0   || { echo "Repository whitelist for $name is expired!"; retcode=1; continue; }
    is_in_transaction $name        || { echo "Repository $name is not in a transaction"; retcode=1; continue; }
    [ $(count_wr_fds /cvmfs/$name) -eq 0 ] || { echo "Open writable file descriptors on $name"; retcode=1; continue; }
    is_cwd_on_path "/cvmfs/$name" && { echo "Current working directory is in /cvmfs/$name.  Please release, e.g. by 'cd \$HOME'."; retcode=1; continue; } || true
    gc_timespan="$(get_auto_garbage_collection_timespan $name)" || { retcode=1; continue; }
    local revision_number=$(get_repo_info -v)
    if [ x"$manual_revision" != x"" ] && [ $manual_revision -le $revision_number ]; then
      echo "Current revision '$revision_number' is ahead of manual revision number '$manual_revision'."
      retcode=1
      continue
    fi

    if [ -z "$tag_name" ] && [ x"$CVMFS_AUTO_TAG" = x"true" ]; then
      local timestamp=$(date -u "+%Y-%m-%dT%H:%M:%SZ")
      tag_name="generic-$timestamp"
      local tag_name_number=1
      while check_tag_existence $name $tag_name; do
        tag_name="generic_$tag_name_number-$timestamp"
        tag_name_number=$(( $tag_name_number + 1 ))
      done
      echo "Using auto tag '$tag_name'"
    fi

    local auto_tag_cleanup_list=
    auto_tag_cleanup_list="$(filter_auto_tags $name)" || { echo "failed to determine outdated auto tags on $name"; retcode=1; continue; }

    # prepare the commands to be used for the publishing later
    local user_shell="$(get_user_shell $name)"

    local base_hash=$(get_mounted_root_hash $name)
    local manifest="${spool_dir}/tmp/manifest"
    local dirtab_command="$(__swissknife_cmd dbg) dirtab \
      -d /cvmfs/${name}/.cvmfsdirtab                     \
      -b $base_hash                                      \
      -w $stratum0                                       \
      -t ${spool_dir}/tmp                                \
      -u /cvmfs/${name}                                  \
      -s ${scratch_dir}                                  \
      $verbosity"

    local log_level=
    [ "x$CVMFS_LOG_LEVEL" != x ] && log_level="-z $CVMFS_LOG_LEVEL"

    local trusted_certs="/etc/cvmfs/repositories.d/${name}/trusted_certs"
    local sync_command="$(__swissknife_cmd dbg) sync \
      -u /cvmfs/$name                                \
      -s ${scratch_dir}                              \
      -c ${spool_dir}/rdonly                         \
      -t ${spool_dir}/tmp                            \
      -b $base_hash                                  \
      -r ${upstream}                                 \
      -w $stratum0                                   \
      -o $manifest                                   \
      -e $hash_algorithm                             \
      -Z $compression_alg                            \
      -C $trusted_certs                              \
      -N $name                                       \
      -K $CVMFS_PUBLIC_KEY                           \
      $(get_follow_http_redirects_flag)              \
      $authz_file                                    \
      $log_level $tweaks_option $external_option $verbosity"
    if [ "x$CVMFS_UNION_FS_TYPE" != "x" ]; then
      sync_command="$sync_command -f $CVMFS_UNION_FS_TYPE"
    fi
    if [ "x$CVMFS_USE_FILE_CHUNKING" = "xtrue" ]; then
      sync_command="$sync_command -p \
       -l $CVMFS_MIN_CHUNK_SIZE \
       -a $CVMFS_AVG_CHUNK_SIZE \
       -h $CVMFS_MAX_CHUNK_SIZE"
    fi
    if [ "x$CVMFS_AUTOCATALOGS" = "xtrue" ]; then
      sync_command="$sync_command -A"
    fi
    if [ "x$CVMFS_AUTOCATALOGS_MAX_WEIGHT" != "x" ]; then
      sync_command="$sync_command -X $CVMFS_AUTOCATALOGS_MAX_WEIGHT"
    fi
    if [ "x$CVMFS_AUTOCATALOGS_MIN_WEIGHT" != "x" ]; then
      sync_command="$sync_command -M $CVMFS_AUTOCATALOGS_MIN_WEIGHT"
    fi
    if [ "x$CVMFS_IGNORE_XDIR_HARDLINKS" = "xtrue" ]; then
      sync_command="$sync_command -i"
    fi
    if [ "x$CVMFS_INCLUDE_XATTRS" = "xtrue" ]; then
      sync_command="$sync_command -k"
    fi
    if [ "x$CVMFS_CATALOG_ENTRY_WARN_THRESHOLD" != "x" ]; then
      sync_command="$sync_command -j $CVMFS_CATALOG_ENTRY_WARN_THRESHOLD"
    fi
    if [ "x$manual_revision" != "x" ]; then
      sync_command="$sync_command -v $manual_revision"
    fi
    if [ "x$CVMFS_REPOSITORY_TTL" != "x" ]; then
      sync_command="$sync_command -T $CVMFS_REPOSITORY_TTL"
    fi
    if [ "x$CVMFS_MAXIMAL_CONCURRENT_WRITES" != "x" ]; then
      sync_command="$sync_command -q $CVMFS_MAXIMAL_CONCURRENT_WRITES"
    fi
    if [ "x${CVMFS_VOMS_AUTHZ}" != x ]; then
      sync_command="$sync_command -V"
    fi
    if [ "x$CVMFS_IGNORE_SPECIAL_FILES" = "xtrue" ]; then
      sync_command="$sync_command -g"
    fi
    local sync_command_virtual_dir=
    if [ "x${CVMFS_VIRTUAL_DIR}" = "xtrue" ]; then
      sync_command_virtual_dir="$sync_command -S snapshots"
    else
      if [ -d /cvmfs/$name/.cvmfs ]; then
        sync_command_virtual_dir="$sync_command -S remove"
      fi
    fi

    local tag_command="$(__swissknife_cmd dbg) tag_edit \
      -r $upstream                                      \
      -w $stratum0                                      \
      -t ${spool_dir}/tmp                               \
      -m $manifest                                      \
      -p /etc/cvmfs/keys/${name}.pub                    \
      -f $name                                          \
      -b $base_hash                                     \
      -e $hash_algorithm                                \
      $(get_follow_http_redirects_flag)                 \
      -x" # -x enables magic undo tag handling
    if [ ! -z "$tag_name" ]; then
      tag_command="$tag_command -a $tag_name"
    fi
    if [ ! -z "$tag_channel" ]; then
      tag_command="$tag_command -c $tag_channel"
    fi
    if [ ! -z "$tag_description" ]; then
      tag_command="$tag_command -D \"$tag_description\""
    fi

    local tag_cleanup_command=
    if [ ! -z "$auto_tag_cleanup_list" ]; then
      tag_cleanup_command="$(__swissknife_cmd dbg) tag_edit \
        -r $upstream                                        \
        -w $stratum0                                        \
        -t ${spool_dir}/tmp                                 \
        -m $manifest                                        \
        -p /etc/cvmfs/keys/${name}.pub                      \
        -f $name                                            \
        -b $base_hash                                       \
        -e $hash_algorithm                                  \
        $(get_follow_http_redirects_flag)                   \
        -d \"$auto_tag_cleanup_list\""
    fi

    # ---> do it! (from here on we are changing things)
    publish_before_hook $name
    $user_shell "$dirtab_command" || die "Failed to apply .cvmfsdirtab"

    # check if we have open file descriptors on /cvmfs/<name>
    local use_fd_fallback=0
    handle_read_only_file_descriptors_on_mount_point $name $open_fd_dialog || use_fd_fallback=1

    # synchronize the repository
    publish_starting $name
    $user_shell "$sync_command" || { publish_failed $name; die "Synchronization failed\n\nExecuted Command:\n$sync_command";   }
    cvmfs_sys_file_is_regular $manifest            || { publish_failed $name; die "Manifest creation failed\n\nExecuted Command:\n$sync_command"; }
    local trunk_hash=$(grep "^C" $manifest | tr -d C)

    # Remove outdated automatically created tags
    if [ ! -z "$tag_cleanup_command" ]; then
      echo "Removing outdated automatically generated tags for $name..."
      $user_shell "$tag_cleanup_command" || { publish_failed $name; die "Removing tags failed\n\nExecuted Command:\n$tag_cleanup_command";  }
      # write intermediate history hash to reflog
      sign_manifest $name $manifest "" true
    fi

    # add a tag for the new revision
    echo "Tagging $name"
    $user_shell "$tag_command" || { publish_failed $name; die "Tagging failed\n\nExecuted Command:\n$tag_command";  }

    if [ "x$sync_command_virtual_dir" != "x" ]; then
      # write intermediate catalog hash and history to reflog
      sign_manifest $name $manifest "" true
      $user_shell "$sync_command_virtual_dir" || { publish_failed $name; die "Editing .cvmfs failed\n\nExecuted Command:\n$sync_command_virtual_dir";  }
      local trunk_hash=$(grep "^C" $manifest | tr -d C)
      $user_shell "$tag_command_undo_tags" || { publish_failed $name; die "Creating undo tags\n\nExecuted Command:\n$tag_command_undo_tags";  }
    fi

    # finalizing transaction
    echo "Flushing file system buffers"
    sync

    # committing newly created revision
    echo "Signing new manifest"
    sign_manifest $name $manifest      || { publish_failed $name; die "Signing failed"; }
    set_ro_root_hash $name $trunk_hash || { publish_failed $name; die "Root hash update failed"; }

    # run the automatic garbage collection (if configured)
    if has_auto_garbage_collection_enabled $name; then
      echo "Running automatic garbage collection"
      local dry_run=0
      __run_gc $name       \
               $stratum0   \
               $dry_run    \
               $manifest   \
               $trunk_hash \
               ""          \
               "0"         \
               -z $gc_timespan      || { local err=$?; publish_failed $name; die "Garbage collection failed ($err)"; }
      sign_manifest $name $manifest || { publish_failed $name; die "Signing failed"; }
    fi

    # check again for open file descriptors (potential race condition)
    if has_file_descriptors_on_mount_point $name && \
       [ $use_fd_fallback -ne 1 ]; then
      file_descriptor_warning $name
      echo "Forcing remount of already committed repository revision"
      use_fd_fallback=1
    else
      echo "Remounting newly created repository revision"
    fi

    # remount the repository
    close_transaction  $name $use_fd_fallback
    publish_after_hook $name
    publish_succeeded  $name

  done

  return $retcode
}


has_file_descriptors_on_mount_point() {
  local name=$1
  local mountpoint="/cvmfs/${name}"

  [ $(count_rd_only_fds $mountpoint) -gt 0 ] || \
  [ $(count_wr_fds      $mountpoint) -gt 0 ]
}


# Lists all auto-generated tags
#
# @param repository_name   the name of the repository to be filtered
# @return                  list of outdated auto-generate tags, space-separated
#              Note: in case of a errors it might print an error to stderr and
#              return a non-zero code
filter_auto_tags() {
  local repository_name="$1"
  local auto_tags_timespan=
  auto_tags_timespan=$(get_auto_tags_timespan "$repository_name") || return 1
  [ $auto_tags_timespan -eq 0 ] && return 0 || true

  load_repo_config $repository_name
  local auto_tags="$(__swissknife tag_list      \
    -w $CVMFS_STRATUM0                         \
    -t ${CVMFS_SPOOL_DIR}/tmp                  \
    -p /etc/cvmfs/keys/${repository_name}.pub  \
    -f $repository_name                        \
    -x $(get_follow_http_redirects_flag)       | \
    grep -E \
    '^generic(_[[:digit:]]+)?-[[:digit:]]{4}-[[:digit:]]{2}-[[:digit:]]{2}T[[:digit:]]{2}:[[:digit:]]{2}:[[:digit:]]{2}Z' | \
    awk '{print $1 " " $5}')"
  [ "x$auto_tags" = "x" ] && return 0 || true

  local tag_name=
  local timestamp=
  local old_tags="$(echo "$auto_tags" | while read tag_name timestamp; do
    if [ "$timestamp" -lt "$auto_tags_timespan" ]; then
      echo -n "$tag_name "
    else
      break
    fi
  done)"
  # Trim old_tags
  echo $old_tags
}


publish_starting() {
  local name=$1
  load_repo_config $name
  local pub_lock="${CVMFS_SPOOL_DIR}/is_publishing"
  acquire_lock "$pub_lock" || die "Failed to acquire publishing lock"
  trap "publish_failed $name" EXIT HUP INT TERM
  run_suid_helper lock $name
  to_syslog_for_repo $name "started publishing"
}


publish_failed() {
  local name=$1
  load_repo_config $name
  local pub_lock="${CVMFS_SPOOL_DIR}/is_publishing"
  trap - EXIT HUP INT TERM
  release_lock $pub_lock
  run_suid_helper open $name
  to_syslog_for_repo $name "failed to publish"
}


publish_succeeded() {
  local name=$1
  load_repo_config $name
  local pub_lock="${CVMFS_SPOOL_DIR}/is_publishing"
  trap - EXIT HUP INT TERM
  release_lock $pub_lock
  to_syslog_for_repo $name "successfully published revision $(get_repo_info -v)"
}

