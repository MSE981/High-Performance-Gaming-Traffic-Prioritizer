import globals from "globals";
import pluginJs from "@eslint/js";

export default [
  {
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
    rules: {
      "semi": ["error", "always"],
      "quotes": ["warn", "single", { "avoidEscape": true }],
      "no-unused-vars": "warn",
      "no-undef": "error"
    }
  }
];
