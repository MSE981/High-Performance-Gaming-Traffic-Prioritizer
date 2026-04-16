import globals from "globals";
import pluginJs from "@eslint/js";

export default [
  {
    files: ["server/**/*.js", "scripts/web/**/*.js", "public/**/*.js"],
    languageOptions: {
      globals: {
        ...globals.node,
        ...globals.browser,
        $: "readonly", // jQuery for EJS frontend scripts
        echarts: "readonly" // echarts for frontend graphs
      }
    }
  },
  pluginJs.configs.recommended,
  {
    files: ["server/**/*.js", "scripts/web/**/*.js", "public/**/*.js"],
    rules: {
      "semi": ["error", "always"],
      "quotes": ["warn", "single", { "avoidEscape": true }],
      "no-unused-vars": "warn",
      "no-undef": "error"
    }
  }
];
