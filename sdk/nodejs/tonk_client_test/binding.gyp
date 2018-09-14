{
  "targets": [{
    "target_name": "tonk",
    "include_dirs" : [
      "src",
      "<!(node -e \"require('nan')\")"
    ],
    "sources": [
      "src/tonk.cc"
    ]
  }]
}
