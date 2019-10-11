#!/usr/bin/env bats

setup() {
  # shellcheck source=/dev/null
  source "${BATS_TEST_DIRNAME}"/getversion
}

@test 'append_to_post_release without a post-release' {
  run append_to_post_release '1.2.3' '4'
  echo "${output}"
  [ "${output}" = '1.2.3+4' ]
}

@test 'append_to_post_release with a post-release' {
  run append_to_post_release '1.2.3+4' '5'
  echo "${output}"
  [ "${output}" = '1.2.3+4.5' ]
}

@test 'drop_commit_sha removes SHA from git describe output' {
  run drop_commit_sha 'tag-123-gabcef'
  echo "${output}"
  [ "${output}" = "tag+123" ]
}

@test 'drop_commit_sha removes SHA from git describe output for a tag with a post_release segment' {
  run drop_commit_sha 'tag+with_post_release-123-gabcef'
  echo "${output}"
  [ "${output}" = "tag+with_post_release.123" ]
}

@test 'drop_commit_sha does not modify git describe output for tag' {
  run drop_commit_sha 'tag'
  echo "${output}"
  [ "${output}" = "tag" ]
}

@test 'drop_commit_sha does not modify git describe output for tag with post_release segment' {
  run drop_commit_sha 'tag+with_post_release'
  echo "${output}"
  [ "${output}" = "tag+with_post_release" ]
}

@test 'generate_build_version() fails with missing params' {
  run generate_build_version
  echo "${output}"
  [ "${status}" -eq 1 ]
  [ "${output}" = 'git_describe_output is missing' ]

  run generate_build_version ''
  echo "${output}"
  [ "${status}" -eq 1 ]
  [ "${output}" = 'post_release_suffix is missing' ]
}

@test 'generate_build_version() does not append an empty suffix to a simple tag' {
  run generate_build_version '6.0.0' ''
  echo "${output}"
  [ "${output}" = '6.0.0' ]
}

@test 'generate_build_version() appends the suffix to a simple tag' {
  run generate_build_version '6.0.0' 'suffix.0'
  echo "${output}"
  [ "${output}" = '6.0.0+suffix.0' ]
}

@test 'generate_build_version() for post release' {
  run generate_build_version '6.0.0+dev.130' 'a.b.0'
  echo "${output}"
  [ "${output}" = '6.0.0+dev.130.a.b.0' ]
}

@test 'generate_build_version() appends the suffix to a tag with a SHA' {
  run generate_build_version '7.0.0-2853-g8fc73277cd' 'suffix.0'
  echo "${output}"
  [ "${output}" = '7.0.0+2853.suffix.0' ]
}

@test 'generate_build_version() for post release with build' {
  run generate_build_version '6.0.0+dev.130-1234-gabcdef' 'a.b.0'
  echo "${output}"
  [ "${output}" = '6.0.0+dev.130.1234.a.b.0' ]
}

@test 'generate_build_version() fails for suffix with a +' {
  run generate_build_version '6.0.0' '+'
  echo "${output}"
  [ "${output}" = 'post_release_suffix cannot contain a +' ]
  [ "${status}" -ne 0 ]

  run generate_build_version '6.0.0' '1+2'
  [ "${status}" -ne 0 ]

  run generate_build_version '6.0.0' '++'
  [ "${status}" -ne 0 ]
}

@test './getversion --build-with-post-release with no parameters passes' {
  run "${BATS_TEST_DIRNAME}"/getversion --build-with-post-release
  echo "${output}"
  [ "${status}" -eq 0 ]
}

@test './getversion --build-with-post-release suffix.0 works' {
  run "${BATS_TEST_DIRNAME}"/getversion --build-with-post-release 'suffix.0'
  echo "${output}"
  [ "${status}" -eq 0 ]
}
