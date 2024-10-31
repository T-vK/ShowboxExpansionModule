module.exports = {
  branches: ["main"],
  plugins: [
    "@semantic-release/commit-analyzer",
    "@semantic-release/release-notes-generator",
    [
      "@semantic-release/github",
      {
        assets: [
          { path: ".pio/build/*/sbxm-firmware-*.bin" },
        ],
      },
    ],
    ["@semantic-release/npm", {
      "npmPublish": false,
    }],
  ],
};
