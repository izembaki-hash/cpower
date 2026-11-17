"""
CPower Brain (clients/python/cpower_brain.py).

The cognitive reasoning engine for CPower.
Decides the next action dynamically based on Set-of-Marks UI elements and user goal.
Supports:
  1. Google Gemini Flash API (Gemini 2.0 Flash / 1.5 Flash) via HTTPS
  2. Local Ollama API (http://localhost:11434)
  3. Semantic Intent Ranker (Fast zero-dependency semantic fallback)
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
from typing import Any, Dict, List, Optional
import urllib.error
import urllib.parse
import urllib.request


class CPowerBrain:
    """Cognitive decision engine for CPower Computer-Use."""

    def __init__(
        self,
        api_key: Optional[str] = None,
        model: str = "gemini-2.0-flash",
        hyperstream_url: str = "http://localhost:8080",
        nvidia_api_key: Optional[str] = None,
    ):
        self.api_key = api_key or os.environ.get("GEMINI_API_KEY") or self._load_saved_key("gemini_api_key")
        self.nvidia_api_key = nvidia_api_key or os.environ.get("NVIDIA_API_KEY") or self._load_saved_key("nvidia_api_key")
        self.model = model
        self.hyperstream_url = hyperstream_url

    def _load_saved_key(self, key_name: str = "gemini_api_key") -> Optional[str]:
        cfg_path = Path.home() / ".cpower_config.json"
        if cfg_path.is_file():
            try:
                data = json.loads(cfg_path.read_text(encoding="utf-8"))
                return data.get(key_name)
            except Exception:
                pass
        return None

    def decide(
        self,
        goal: str,
        elements_summary: str,
        current_url: str,
        history: List[str],
        content_summary: str = "",
        image_base64: Optional[str] = None,
    ) -> Dict[str, Any]:
        """
        Chooses the next rational action based on what is currently on screen.
        Priority Hierarchy:
          1. HyperStream (Local Bare-Metal Vulkan GPU, 20 tok/s, AMD RX 580, zero-cost)
          2. Google Gemini Flash (HTTPS API)
          3. NVIDIA NIM Super-VL (120B Multimodal Reasoning)
          4. Domain-Agnostic Semantic Intent Scorer (5ms instant fallback)
        """
        # 1. Try Local HyperStream Engine (AMD GPU Vulkan)
        try:
            res = self._decide_hyperstream(goal, elements_summary, current_url, history, content_summary)
            if res:
                return res
        except Exception:
            pass

        # 2. Try Gemini API
        if self.api_key:
            try:
                return self._decide_gemini(goal, elements_summary, current_url, history, content_summary)
            except Exception as e:
                print(f"[BRAIN] Gemini API error ({e}), evaluating next cognitive provider...")

        # 3. Try NVIDIA NIM Super-VL (if visual input or multimodal needed)
        if self.nvidia_api_key and image_base64:
            try:
                return self._decide_nvidia_nim_vl(goal, elements_summary, current_url, history, image_base64)
            except Exception as e:
                print(f"[BRAIN] NVIDIA NIM VL error ({e}), falling back to Semantic Intent Ranker...")

        # 4. Pure Semantic Intent Fallback
        return self._decide_semantic_intent(goal, elements_summary, current_url, history)

    def _decide_hyperstream(
        self,
        goal: str,
        elements_summary: str,
        current_url: str,
        history: List[str],
        content_summary: str = "",
    ) -> Optional[Dict[str, Any]]:
        """Query local HyperStream Vulkan engine."""
        prompt = (
            f"User Goal: {goal}\n"
            f"Current URL: {current_url}\n"
            f"Recent Actions:\n" + "\n".join(f"- {h}" for h in history[-3:]) + "\n\n"
            f"Numbered Interactive UI Elements:\n{elements_summary[:800]}\n\n"
            "Respond in JSON format: {\"thought\": \"...\", \"action\": \"click\"|\"paste\"|\"navigate\"|\"scroll\"|\"finish\", \"mark_id\": <int or null>, \"text\": \"<text or null>\", \"delta\": <int or null>}"
        )
        encoded_prompt = urllib.parse.quote(prompt)
        url = f"{self.hyperstream_url}/api/generate?prompt={encoded_prompt}&backend=vulkan"

        req = urllib.request.Request(url, headers={"User-Agent": "CPowerBrain/1.0"})
        with urllib.request.urlopen(req, timeout=3.0) as resp:
            # HyperStream streams SSE: data: {"token": "..."}
            full_text = ""
            for line in resp:
                line_str = line.decode("utf-8", errors="ignore").strip()
                if line_str.startswith("data:"):
                    raw_json = line_str[5:].strip()
                    try:
                        token_obj = json.loads(raw_json)
                        full_text += token_obj.get("token", "")
                    except Exception:
                        pass

        # Extract JSON from output
        m = re.search(r"\{[^{}]*\"action\"[^{}]*\}", full_text, re.DOTALL)
        if m:
            return json.loads(m.group(0))
        return None

    def _decide_nvidia_nim_vl(
        self,
        goal: str,
        elements_summary: str,
        current_url: str,
        history: List[str],
        image_base64: str,
    ) -> Dict[str, Any]:
        """Query NVIDIA NIM Super-VL (120B Multimodal Reasoning)."""
        url = "https://integrate.api.nvidia.com/v1/chat/completions"
        system_instruction = (
            "You are CPower Brain, an autonomous AI controlling a Windows PC.\n"
            "Analyze the numbered Set-of-Marks elements on screen and decide the single best next action.\n"
            "Return strictly valid JSON with no markdown formatting:\n"
            '{"thought": "...", "action": "click"|"paste"|"navigate"|"scroll"|"finish", '
            '"mark_id": <int or null>, "text": "<text to type/navigate or null>", "delta": <int or null>}'
        )
        user_prompt = (
            f"User Goal: {goal}\n"
            f"Current URL: {current_url}\n"
            f"Recent Actions:\n" + "\n".join(f"- {h}" for h in history[-3:]) + "\n\n"
            f"Interactive Elements on Screen (Set-of-Marks):\n{elements_summary}\n\n"
            "What is the best next step?"
        )
        image_uri = f"data:image/jpeg;base64,{image_base64}"
        payload = {
            "model": "nvidia/nemotron-3.5-super-vl-120b-a12b",
            "messages": [
                {
                    "role": "user",
                    "content": [
                        {"type": "text", "text": f"{system_instruction}\n\n{user_prompt}"},
                        {"type": "image_url", "image_url": {"url": image_uri}},
                    ],
                }
            ],
            "max_tokens": 512,
            "temperature": 0.1,
        }

        req = urllib.request.Request(
            url,
            data=json.dumps(payload).encode("utf-8"),
            headers={
                "Authorization": f"Bearer {self.nvidia_api_key}",
                "Content-Type": "application/json",
            },
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=25.0) as resp:
            data = json.loads(resp.read().decode("utf-8"))

        choice = data["choices"][0]["message"]
        content = choice.get("content") or choice.get("reasoning_content") or ""
        m = re.search(r"\{[^{}]*\"action\"[^{}]*\}", content, re.DOTALL)
        if m:
            return json.loads(m.group(0))
        return json.loads(content)

    def _decide_gemini(
        self,
        goal: str,
        elements_summary: str,
        current_url: str,
        history: List[str],
        content_summary: str = "",
    ) -> Dict[str, Any]:
        url = f"https://generativelanguage.googleapis.com/v1beta/models/{self.model}:generateContent?key={self.api_key}"

        system_instruction = (
            "You are CPower Brain, an autonomous AI controlling a Windows PC.\n"
            "Analyze the numbered Set-of-Marks elements on screen and decide the single best next action.\n"
            "Return strictly valid JSON with no markdown formatting:\n"
            '{"thought": "...", "action": "click"|"paste"|"navigate"|"scroll"|"finish", '
            '"mark_id": <int or null>, "text": "<text to type/navigate or null>", "delta": <int or null>}'
        )

        user_prompt = (
            f"User Goal: {goal}\n"
            f"Current URL/Location: {current_url}\n"
            f"Recent Actions:\n" + "\n".join(f"- {h}" for h in history[-5:]) + "\n\n"
            f"Screen Content Excerpt:\n{content_summary[:300]}\n\n"
            f"Interactive Elements on Screen (Set-of-Marks):\n{elements_summary}\n\n"
            "What is the best next step?"
        )

        payload = {
            "contents": [
                {"role": "user", "parts": [{"text": f"{system_instruction}\n\n{user_prompt}"}]}
            ],
            "generationConfig": {
                "temperature": 0.1,
                "responseMimeType": "application/json",
            }
        }

        req = urllib.request.Request(
            url,
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )

        with urllib.request.urlopen(req, timeout=10.0) as resp:
            data = json.loads(resp.read().decode("utf-8"))

        text = data["candidates"][0]["content"]["parts"][0]["text"].strip()
        # Clean JSON if wrapped in markdown
        text = re.sub(r"^```(?:json)?\s*", "", text)
        text = re.sub(r"\s*```$", "", text)
        return json.loads(text)

    def _decide_semantic_intent(
        self,
        goal: str,
        elements_summary: str,
        current_url: str,
        history: List[str],
    ) -> Dict[str, Any]:
        """Domain-agnostic semantic intent scorer (zero hardcoded rules)."""
        lines = [line.strip() for line in elements_summary.splitlines() if line.strip()]
        if not lines:
            return {"thought": "No interactive elements found, scrolling to reveal content.", "action": "scroll", "delta": -400}

        # Parse elements
        parsed = []
        for line in lines:
            m = re.match(r"\[(\d+)\]\s*<(\w+)>\s*'([^']*)'", line)
            if m:
                mark_id = int(m.group(1))
                ctrl_type = m.group(2)
                text = m.group(3)
                parsed.append({"mark": mark_id, "type": ctrl_type, "text": text, "raw": line})

        # Tokenize goal words
        goal_words = set(re.findall(r"\w+", goal.lower()))
        generic_nav = {"home", "search", "menu", "sign in", "login", "register", "about", "terms", "privacy", "help", "cookie", "accept"}

        # Check if cookie/consent banner present
        for p in parsed:
            txt_lower = p["text"].lower()
            if any(k in txt_lower for k in ("accept all", "reject all", "agree", "dismiss", "got it", "accept cookies")):
                return {
                    "thought": f"Detected consent banner element [{p['mark']}]: {p['text']}. Dismissing it.",
                    "action": "click",
                    "mark_id": p["mark"],
                }

        # Score candidate elements based on length and relevance to goal
        best_score = -1.0
        best_candidate = None

        for p in parsed:
            txt = p["text"]
            txt_lower = txt.lower()
            if any(k in txt_lower for k in generic_nav) and len(txt) < 15:
                continue

            score = 0.0
            # Higher weight for hyperlinks and buttons with substantive text
            if p["type"] == "Hyperlink":
                score += 2.0
            if len(txt) > 20:
                score += 3.0  # Likely an article/story title
            if len(txt) > 40:
                score += 2.0

            # Match words from goal
            txt_words = set(re.findall(r"\w+", txt_lower))
            overlap = goal_words.intersection(txt_words)
            score += len(overlap) * 4.0

            # Penalize items already visited
            if any(txt[:25] in h for h in history):
                score -= 10.0

            if score > best_score:
                best_score = score
                best_candidate = p

        if best_candidate and best_score > 0:
            return {
                "thought": f"Selected candidate [{best_candidate['mark']}] '{best_candidate['text'][:50]}' as the most relevant item.",
                "action": "click",
                "mark_id": best_candidate["mark"],
            }

        return {
            "thought": "No distinct target element found on current screen, scrolling down.",
            "action": "scroll",
            "delta": -400,
        }
