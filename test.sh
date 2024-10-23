#!/bin/sh
if [[ "pull_request_target" == "pull_request"* ]]; then
  echo true
else
  echo false
fi

